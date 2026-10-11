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


project_location = "/Users/julian/Library/CloudStorage/OneDrive-HAW-HH/Projekte/Digitale Filmklappe/Software/READ_ARC/test_04/"


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

def read_qr_from_video(video, video_path=None):
    qr_detector = cv2.QRCodeDetector()
    frame_count = 0
    found = False
    bboxes = []
    while True:
        ret, frame = video.read()
        if not ret:
            break  # End of video
        frame_count += 1
        data, bbox, _ = qr_detector.detectAndDecodeCurved(frame)
        if data is not None:
            print(f"QR Code found in frame {frame_count}: {data}")
            found = True
    video.release()
    if found:
        return data
    else:
        print("No QR code found in the video.")
        return None

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
    global current_code, current_roll, current_scene, current_shot, current_take
    if qr_code_data:
        parts = qr_code_data.split('_')
        if len(parts) == 5:
            current_code, current_roll, current_scene, current_shot, current_take = parts
            print(f"Code: {current_code}, Roll: {current_roll}, Scene: {current_scene}, Shot: {current_shot}, Take: {current_take}")
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
            if shot.get("shot") == current_shot and shot.get("scene") == current_scene
            for take in shot.get("takes", [])
                if take.get("take") == int(current_take)
                and take.get("roll") == current_roll),
        "00:00:00:00" 
    )
    framerate = next(
        (shot["framerate"]
        for shot in json_meta["shotlist"]
            if shot.get("shot") == current_shot and shot.get("scene") == current_scene),
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
        "Comments",
        "Good Take",
        "Director",
        "DOP"
    ]
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
            if shot_num == current_shot and scene == current_scene:
                for take in shot.get("takes", []):
                    take_num = str(take.get("take", ""))
                    roll = take.get("roll", "")
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
                            "Comments": comments,
                            "Good Take": good_take,
                            "Director": director,
                            "DOP": dop,
                        }
                        writer.writerow(row)
                        print(f"CSV file created at {csv_file_path}")
                        break
                break

def get_point_near_qr(box, rel_x, rel_y):
    box = np.array(box).reshape(4, 2)
    top = box[0] + rel_x * (box[1] - box[0])
    bottom = box[3] + rel_x * (box[2] - box[3])
    point = top + rel_y * (bottom - top)
    return tuple(point)

def read_qr_bbox_per_frame(video_path):
    qr_detector = cv2.QRCodeDetector()
    cap = cv2.VideoCapture(video_path)
    frame_count = 0
    qr_data = None
    bboxes = []
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        frame_count += 1
        data, bbox, _ = qr_detector.detectAndDecodeCurved(frame)
        if bbox is not None:
            bboxes.append((frame_count-1, bbox))
            if qr_data is None and data:
                qr_data = data
    cap.release()
    if qr_data is None:
        print("No QR code found in the video.")
    return qr_data, bboxes

def get_roi_from_bbox(bbox, rel_x, rel_y):
    box = np.array(bbox).reshape(4, 2)
    pt1 = box[0]
    pt2 = box[1]
    size = int(np.linalg.norm(pt2 - pt1)/4)
    px, py = get_point_near_qr(bbox, rel_x, rel_y)
    x1 = int(px - size // 2)
    y1 = int(py - size // 2)
 
    return x1, y1, size, size

def detect_bright_frame_with_dynamic_roi(video_path, bboxes, rel_x, rel_y, createvideo=False):
    bbox_dict = {idx: box for idx, box in bboxes}
    cap = cv2.VideoCapture(video_path)
    frame_num = 0
    last_bbox = None
    last_roi = None
    bright_frame = None

    # VideoWriter vorbereiten, falls benötigt
    if createvideo:
        fourcc = cv2.VideoWriter_fourcc(*'mp4v')
        fps = cap.get(cv2.CAP_PROP_FPS)
        width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        out_path = video_path.replace('.MOV', '_roi.mp4').replace('.mov', '_roi.mp4')
        out = cv2.VideoWriter(out_path, fourcc, fps, (width, height))
    else:
        out = None

    while True:
        ret, frame = cap.read()
        if not ret:
            break
        bbox = bbox_dict.get(frame_num, last_bbox)
        if bbox is None:
            frame_num += 1
            if out is not None:
                out.write(frame)
            continue
        last_bbox = bbox
        x, y, w, h = get_roi_from_bbox(bbox, rel_x, rel_y)
        # Stabilisierung: ROI mit vorherigem Wert mitteln
        if last_roi is not None:
            x = int((x + last_roi[0]) / 2)
            y = int((y + last_roi[1]) / 2)
        last_roi = (x, y)
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        roi_img = gray[y:y+h, x:x+w]
        # ROI einzeichnen, falls Video erstellt werden soll
        if out is not None:
            frame_with_rect = frame.copy()
            cv2.rectangle(frame_with_rect, (x, y), (x + w, y + h), (0, 0, 255), 4)
            out.write(frame_with_rect)
        if roi_img.size == 0:
            frame_num += 1
            continue
        if np.mean(roi_img > 220) > 0.1:  # Helligkeitsschwelle: höher als 230 von 255 bei 10% der Fläche des ROIs
            bright_frame = frame_num + 1
            print(f"Bright frame detected at frame {bright_frame}")
            break
        frame_num += 1

    cap.release()
    if out is not None:
        out.release()
        print(f"Video mit ROI gespeichert: {out_path}")

    if bright_frame is not None:
        return bright_frame - 1
    print("No bright frame detected.")
    return 0

# --- Hauptablauf ---
import_folder = os.path.join(project_location, "01_IMPORT")
video_files = sorted(glob.glob(os.path.join(import_folder, "*.mov"))) + \
              sorted(glob.glob(os.path.join(import_folder, "*.MOV")))

if not video_files:
    print("Error: No video files found in 01_IMPORT folder.")
    sys.exit(1)

for video_path in video_files:
    print(f"\n--- Verarbeitung von: {video_path} ---")
    qr_code_data, bboxes = read_qr_bbox_per_frame(video_path)
    if qr_code_data is None:
        continue
    # Optional: Video mit Punkt zeichnen
    if bboxes:
        sync_frame = detect_bright_frame_with_dynamic_roi(video_path, bboxes, -1.77, -0.25, False)
    else:
        sync_frame = 0
    cap = cv2.VideoCapture(video_path)
    framecount = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    cap.release()
    add_metadata(project_location, qr_code_data, sync_frame, framecount, video_path)