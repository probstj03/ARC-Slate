import cv2
import sys
import json
import subprocess
from pathlib import Path
import os
import glob
import csv
from datetime import datetime
import numpy as np
from pyzbar.pyzbar import decode
import warnings
warnings.filterwarnings("ignore")

print("Script started")

project_location = '/Volumes/JULIAN_SSD/Videos/2025 Schlüsselloch/ARC-SLATE-0005'

def order_polygon_points(pts):
    """
    Sortiere die 4 Punkte eines Quadrats:
    Reihenfolge: oben-links, unten-left, unten-rechts, oben-rechts
    """
    # konvertiere in (4,2)
    pts = np.array(pts, dtype=np.float32).reshape(-1, 2)

    # Summe und Differenz der Koordinaten
    s = pts.sum(axis=1)
    diff = np.diff(pts, axis=1).reshape(-1)

    ordered = np.zeros((4,2), dtype=np.float32)
    ordered[0] = pts[np.argmin(s)]      # oben-links: kleinste Summe
    ordered[2] = pts[np.argmax(s)]      # unten-rechts: größte Summe
    ordered[1] = pts[np.argmin(diff)]   # oben-rechts: kleinste Differenz
    ordered[3] = pts[np.argmax(diff)]   # unten-links: größte Differenz

    return ordered

def enhance_contrast(frame):

    # Kontrast- und Gamma-Anpassung

    frame_float = frame.astype(np.float32) / 255.0
    gamma = 1.2 # Gamma <1 hebt Mitteltöne an
    frame_gamma = np.power(frame_float, gamma)
    alpha = 2  # Kontrastfaktor
    frame_enhanced = np.clip(frame_gamma * alpha, 0, 1)
    frame_enhanced = (frame_enhanced * 255).astype(np.uint8)

    return frame_enhanced

def open_video_file(location):
    # Find the first video file in the 01_IMPORT folder

    import_folder = os.path.join(location, "01_IMPORT")
    video_files = sorted(glob.glob(os.path.join(import_folder, "*.mov"))) + \
                  sorted(glob.glob(os.path.join(import_folder, "*.MOV")))
    if not video_files:
        print("Error: No video files found in 01_IMPORT folder.")
        sys.exit(1)
    video = cv2.VideoCapture(video_files[0])
    if not video.isOpened():
        print(f"Error: Cannot open video file {video_files[0]}.")
        sys.exit(1)
    print(f"Opened video file: {video_files[0]}")
    return video

def adjust_timecode(timecode, frames_delta, fps):
    """
    Add or subtract frames from a timecode string "HH:MM:SS:FF".
    frames_delta: positive to add, negative to subtract.
    """
    h, m, s, f = map(int, timecode.split(":"))
    total_frames = ((h * 3600 + m * 60 + s) * fps) + f
    new_total_frames = max(0, total_frames + frames_delta)
    nh = new_total_frames // (3600 * fps)
    nm = (new_total_frames % (3600 * fps)) // (60 * fps)
    ns = (new_total_frames % (60 * fps)) // fps
    nf = new_total_frames % fps
    return f"{nh:02}:{nm:02}:{ns:02}:{nf:02}"

def add_metadata(location, qr_code_data, sync_frame, framecount, input_mov_path):
    # --- QR-Code dekodieren ---
    global current_code, current_version, current_roll, current_scene, current_shot, current_take
    if qr_code_data:
        parts = qr_code_data.split('_')
        if len(parts) == 6:
            current_code, current_version, current_roll, current_scene, current_shot, current_take = parts
            print(f"Code: {current_code}, Version: {current_version}, Roll: {current_roll}, Scene: {current_scene}, Shot: {current_shot}, Take: {current_take}")
        else:
            print("QR code data format is incorrect.")
            return
    else:
        print("No QR code data to decode.")
        return

    # --- JSON-Datei öffnen ---
    try:
        json_folder = os.path.join(location, "02_JSONFILE")
        json_files = sorted(glob.glob(os.path.join(json_folder, "*.json")))
        if not json_files:
            print("No JSON files found in 02_JSONFILE folder.")
            return
        with open(json_files[0], 'r') as f:
            json_meta = json.load(f)
            print(f"Opened JSON file: {json_files[0]}")
    except Exception as e:
        print(f"Error opening JSON file: {e}")
        return

    # --- Timecode einbetten ---
    sync_timecode = next(
        (take["timecode"]
        for shot in json_meta["shotlist"]
            if shot.get("shot") == current_shot and shot.get("scene") == int(current_scene)
            for take in shot.get("takes", [])
                if take.get("take") == int(current_take)
                and take.get("roll") == current_roll),
        "00:00:00:00" 
    )
    framerate = next(
        (shot["framerate"]
        for shot in json_meta["shotlist"]
            if shot.get("shot") == current_shot and shot.get("scene") == int(current_scene)),
        24
    )
    print("Sync TC: " + sync_timecode)
    start_timecode = adjust_timecode(sync_timecode, - sync_frame -1, framerate)
    print("Start TC: " + start_timecode)

    # project_name aus location extrahieren
    project_name = os.path.basename(os.path.normpath(location))

    input_mov = input_mov_path
    output_mov = os.path.join(location, "03_EXPORT", current_code, "SCENE_" + current_scene, "SHOT_" + current_shot, f"{current_code}_{current_roll}_{current_scene}_{current_shot}_TAKE_{current_take}.mov")
    output_dir = os.path.dirname(output_mov)
    os.makedirs(output_dir, exist_ok=True)

    # Aufnahme-Datum und -Zeit aus JSON holen (oder Systemzeit als Fallback)
    recording_date = None
    for shot in json_meta.get("shotlist", []):
        if shot.get("shot") == current_shot:
            for take in shot.get("takes", []):
                if take.get("take") == int(current_take):
                    date_str = take.get("date")
                    if date_str:
                        # Umwandlung von DD.MM.YYYY nach YYYY-MM-DD
                        try:
                            dt = datetime.strptime(date_str, "%d.%m.%Y")
                            recording_date = dt.strftime("%Y-%m-%d")
                        except Exception as e:
                            print(f"Fehler beim Umwandeln des Datums: {e}")
                            recording_date = None
                    break
            if recording_date:
                break
    recording_time = start_timecode.split(":")
    recording_time = ":".join(recording_time[:3])
    creation_time = f"{recording_date}T{recording_time}"

    ffmpeg_cmd = [
        "ffmpeg",
        "-y",
        "-i", input_mov,
        "-timecode", start_timecode,
        "-metadata", f"creation_time={creation_time}",
        "-c", "copy",
        output_mov
    ]
    result = subprocess.run(ffmpeg_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Fehler beim Hinzufügen des Timecodes und Datums:")
        print(result.stderr)
        # raise RuntimeError("ffmpeg fehlgeschlagen")
    else:
        print(f"Timecode {start_timecode} und creation_time {creation_time} erfolgreich zu {output_mov} hinzugefügt.")
    # --- CSV erzeugen ---
    csv_folder = os.path.join(location, "04_CSVFILE")
    os.makedirs(csv_folder, exist_ok=True)
    csv_file_path = os.path.join(csv_folder, f"{project_name} Medienmetadaten.csv")
    fieldnames = [
        "File Name",
        "Clip Directory",
        "Scene",
        "Shot",
        "Take",
        "Day / Night",
        "Environment",
        "Date Recorded",
        "Roll Card #",
        "Start TC",
        "End TC",
        "Description",
        "Comments",
        "Good Take",
        "Production Name",
        "Director",
        "DOP"
    ]
    production = json_meta.get("production", "")
    director = json_meta.get("director", "")
    dop = json_meta.get("dop", "")
    
    
    with open(csv_file_path, 'a', newline='') as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if os.stat(csv_file_path).st_size == 0:
            writer.writeheader()
        for shot in json_meta.get("shotlist", []):
            scene = shot.get("scene", "")
            shot_num = shot.get("shot", "")
            daynight = shot.get("daynight", "")
            environment = shot.get("environment", "")
            if shot_num == current_shot and scene == int(current_scene):
                for take in shot.get("takes", []):
                    take_num = str(take.get("take", ""))
                    roll = take.get("roll", "")
                    description = shot.get("description", "")
                    if take_num == str(current_take) and roll == current_roll:
                        file_name = f"{current_code}_{roll}_{scene}_{current_shot}_TAKE_{current_take}.mov"
                        clip_dir = f"{location}03_EXPORT/{current_code}/SCENE_{scene}/SHOT_{shot_num}"
                        date = take.get("date", "")
                        comments = take.get("notes", "")
                        good_take = 1 if take.get("goodTake") else 0
                        row = {
                            "File Name": file_name,
                            "Clip Directory": clip_dir,
                            "Scene": scene,
                            "Shot": shot_num,
                            "Take": take_num,
                            "Day / Night": daynight,
                            "Environment": environment,
                            "Date Recorded": date,
                            "Roll Card #": roll,
                            "Start TC": start_timecode,
                            "End TC": adjust_timecode(start_timecode, framecount, framerate),
                            "Description": description,
                            "Comments": comments,
                            "Good Take": good_take,
                            "Production Name": production,
                            "Director": director,
                            "DOP": dop,
                        }
                        writer.writerow(row)
                        print(f"CSV file created at {csv_file_path}")
                        break
                break

def read_qr_bbox_per_frame(video_path, show_frame=False):
    cap = cv2.VideoCapture(video_path)
    frame_count = 0
    qr_data = None
    sync_frame = 0
    bboxes = []
    is_bright = False
    while True:
        ret, frame = cap.read()
        data = None
        polygon = 0
        if not ret:
            break
        frame_count += 1
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)  
        enhanced_frame = enhance_contrast(gray)
        decoded_objects = decode(enhanced_frame)
        if show_frame:
            cv2.imwrite('debug_frame.jpg', enhanced_frame)
            cv2.imshow('Enhanced Frame', enhanced_frame)
        cv2.waitKey(1)  # nicht blockierend; für interaktives Debug ggf. 0
        
        if len(bboxes) < frame_count:
            # Liste bei Bedarf auffüllen
            bboxes.append(None)

        for obj in decoded_objects:
            data = obj.data.decode("utf-8")
            polygon = obj.polygon
                
            if data:
                qr_data = data
            else:
                qr_data = "NONE"                
            print(f"QR code found at frame {frame_count}: {qr_data}")


        if polygon != 0:
            bbox = np.array([[p.x, p.y] for p in polygon], dtype=np.int32)
            bbox = order_polygon_points(bbox)
        else:
            if len(bboxes) > 0:
                bbox = bboxes[-1]  # Letzte verfügbare bbox verwenden
            else:
                bbox = np.zeros((4, 2), dtype=np.int32)

        bboxes.append(bbox)
        
        if bboxes[-1] is not None:

            is_bright = detect_bright_frame_with_dynamic_roi(frame, bboxes, frame_count)

        if is_bright:
            sync_frame = frame_count
            cap.release()
            return sync_frame, qr_data

    cap.release()
    if qr_data is None:
        print("No QR code found in the video.")

    return sync_frame, qr_data

def detect_bright_frame_with_dynamic_roi(frame, bboxes, frame_count, show_Frame=False):
    
    
    
    if len(bboxes) < 3:
        bbox = bboxes[-1]  # nimm die letzte bekannte Box
    else:
        bbox_second_last = bboxes[-3] if isinstance(bboxes[-3], np.ndarray) else np.zeros((4, 2), dtype=np.float32)
        bbox_last = bboxes[-2] if isinstance(bboxes[-2], np.ndarray) else np.zeros((4, 2), dtype=np.float32)
        bbox_current = bboxes[-1] if isinstance(bboxes[-1], np.ndarray) else np.zeros((4, 2), dtype=np.float32)

        bbox = np.mean(np.stack([bbox_second_last, bbox_last, bbox_current]), axis=0).astype(np.float32)

    if bbox is None:
        return False
        
    normalized = np.array([
    [0, 0],
    [0, 1],
    [1, 1],
    [1, 0]
    ], dtype=np.float32)
    
    try:
        H, _ = cv2.findHomography(normalized, bbox)
    except cv2.error:
        return False
    
    
    offset_x = -0.8
    offset_y = -2.2
    
    new_norm = np.array([
        [0.35+offset_x, 0.35+offset_y],
        [0.35+offset_x, 0.65+offset_y],
        [0.65+offset_x, 0.65+offset_y],
        [0.65+offset_x, 0.35+offset_y]
        ], dtype=np.float32)
    
    
    new_norm = np.float32([new_norm])
    roi = cv2.perspectiveTransform(new_norm.reshape(-1, 1, 2).astype(np.float32), H)

    

    # Optional: zeichnen / debug
    if show_Frame:
        frame_with_rect = frame.copy()
        roi_int = roi.reshape(-1, 1, 2).astype(np.int32)
        cv2.polylines(frame_with_rect, [roi_int], isClosed=True, color=(0,0,255), thickness=3)
        cv2.imwrite('debug_roi.jpg', frame_with_rect)
        cv2.imshow('Frame with ROI', frame_with_rect)
        cv2.waitKey(1)  # nicht blockierend; für interaktives Debug ggf. 0

    # Maske erstellen
    mask = np.zeros(frame.shape[:2], dtype=np.uint8)

    # roi = aus perspectiveTransform
    roi_int = roi.reshape(-1, 2).astype(np.int32)

    # Polygon füllen
    cv2.fillPoly(mask, [roi_int], 255)

    # Grauwertbild des gesamten Frames
    frame_gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # Nur die Pixel innerhalb der ROI auswählen
    roi_gray_pixels = frame_gray[mask == 255]

    # Helligkeitsprüfung (z. B. Pixel > 220)
    is_bright = np.mean(roi_gray_pixels > 220) > 0.1
    return is_bright

# --- Hauptablauf ---
import_folder = os.path.join(project_location, "01_IMPORT")
video_files = sorted(glob.glob(os.path.join(import_folder, "*.mov"))) + \
              sorted(glob.glob(os.path.join(import_folder, "*.MOV")))

if not video_files:
    print("Error: No video files found in 01_IMPORT folder.")
    sys.exit(1)

for video_path in video_files:
    print(f"\n--- Verarbeitung von: {video_path} ---")
    sync_frame, qr_code_data = read_qr_bbox_per_frame(video_path)
    print(sync_frame)
    add_metadata(project_location, qr_code_data, sync_frame, 0, video_path)