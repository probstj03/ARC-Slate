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
from qreader import QReader
import easyocr

from slate_detection.slate_detector import SlateDetector
import re

project_location = '/Volumes/JULIAN_SSD/Videos/2025 Schlüsselloch/ARC-SLATE-0005'


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

def detect_bright_frame(frame, roi):
    roi_points = roi.reshape(-1, 2).astype(np.int32)

    # Extract ROI from frame
    mask = np.zeros(frame.shape[:2], dtype=np.uint8)
    cv2.fillPoly(mask, [roi_points], 255)
    roi_pixels = frame[mask == 255]
    
    if roi_pixels.size == 0:
        return False
    
    mean_brightness = np.mean(roi_pixels)
    threshold = 200
    
    return mean_brightness > threshold

def get_dynamic_roi(bboxes, slate_bbox, frame_count, lookback):
    if frame_count < lookback:
        valid_bboxes = [bbox for bbox in bboxes[:frame_count] if bbox is not None]
    else:
        valid_bboxes = [bbox for bbox in bboxes[frame_count - lookback:frame_count] if bbox is not None]
    
    if not valid_bboxes:
        return None
    
    avg_bbox = np.mean(valid_bboxes, axis=0)
    
    normalized = np.array([
    [0, 0],
    [0, 1],
    [1, 1],
    [1, 0]
    ], dtype=np.float32)
    
    try:
        H, _ = cv2.findHomography(normalized, avg_bbox)
    except cv2.error:
        return False
    
    # Roi for flash
    offset_x = -0.9
    offset_y = -2.3
    
    new_norm = np.array([
        [0.30+offset_x, 0.30+offset_y],
        [0.30+offset_x, 0.70+offset_y],
        [0.70+offset_x, 0.70+offset_y],
        [0.70+offset_x, 0.30+offset_y]
        ], dtype=np.float32)
     
    
    new_norm = np.float32([new_norm])
    
    roi = cv2.perspectiveTransform(new_norm, H)
    
    # Initialisiere Tracker, wenn ROI stabil ist
    if len(bboxes) >= 5:  # Mindestens 5 Frames für Stabilitätscheck
        recent_rois = [get_dynamic_roi(bboxes, slate_bbox, i, 3) 
                       for i in range(max(0, frame_count - 5), frame_count)
                       if get_dynamic_roi(bboxes, slate_bbox, i, 3) is not None]
        
        if len(recent_rois) >= 3:
            # Berechne Varianz der ROI-Positionen
            roi_centers = [np.mean(r.reshape(-1, 2), axis=0) for r in recent_rois]
            roi_variance = np.var(roi_centers, axis=0)
            
            # ROI ist stabil, wenn Varianz klein ist
            if np.all(roi_variance < 100):  # Threshold für Stabilität
                print("ROI stabil - Tracker aktiviert")

    return roi

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

def create_videofile(input_mov, output_mov, start_timecode, creation_time, start_frame, end_frame, framerate):
    # Berechne Startzeit und Dauer in Sekunden basierend auf Frames und Framerate
    print(f"Schneide Video von Frame {start_frame} bis {end_frame} mit Framerate {framerate} fps.")
    start_sec = start_frame / framerate
    duration_sec = (end_frame - start_frame + 1) / framerate

    ffmpeg_cmd = [
        "ffmpeg",
        "-y",
        "-i", input_mov,
        "-ss", f"{start_sec:.3f}",
        "-t", f"{duration_sec:.3f}",
        "-timecode", start_timecode,
        "-metadata", f"creation_time={creation_time}",
        "-c", "copy",
        output_mov
    ]
    result = subprocess.run(ffmpeg_cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print("Fehler beim Schneiden und Hinzufügen des Timecodes und Datums:")
        print(result.stderr)
    else:
        print(f"Video von Frame {start_frame} bis {end_frame} mit Timecode {start_timecode} und creation_time {creation_time} erfolgreich zu {output_mov} exportiert.")

def create_csv(csv_file_path, fieldnames, json_meta, current_code, current_roll, current_scene, current_shot, current_take, start_timecode, framecount, framerate, location):
    production = json_meta.get("production", "")
    director = json_meta.get("director", "")
    dop = json_meta.get("dop", "")
    rows = []
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
                    rows.append(row)
                    break
            break
    with open(csv_file_path, 'a', newline='') as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        if os.stat(csv_file_path).st_size == 0:
            writer.writeheader()
        for row in rows:
            writer.writerow(row)
        print(f"CSV file created at {csv_file_path}")

def create_video_with_metadata(video_path, qr_code_data, sync_frame, start_frame, end_frame):
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
        json_folder = os.path.join(project_location, "02_JSONFILE")
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
    if sync_frame is not None:
        delta_frames = max(0, -sync_frame - 1)
    else:
        delta_frames = 0
    start_timecode = adjust_timecode(sync_timecode, delta_frames, framerate)
    print("Start TC: " + start_timecode)

    input_mov = video_path
    output_mov = os.path.join(project_location, "03_EXPORT", current_code, "SCENE_" + current_scene, "SHOT_" + current_shot, f"{current_code}_{current_roll}_{current_scene}_{current_shot}_TAKE_{current_take}.mov")
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

    create_videofile(input_mov, output_mov, start_timecode, creation_time,  start_frame, end_frame, framerate)

    # --- CSV erzeugen ---
    csv_folder = os.path.join(project_location, "04_CSVFILE")
    os.makedirs(csv_folder, exist_ok=True)
    project_name = os.path.basename(os.path.normpath(project_location))
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
    create_csv(csv_file_path, fieldnames, json_meta, current_code, current_roll, current_scene, current_shot, current_take, start_timecode, end_frame - start_frame, framerate, project_location)

def process_video(video_path, show_frame=False):
    cap = cv2.VideoCapture(video_path)
    frame_count = 0
    qreader = QReader('l')
    qr_data = None
    slate_polygon = None
    polygon = None
    roi = None
    bright_frame = None
    value_buffer = {
        'roll': [],
        'cam': [],
        'scene': [],
        'take': [],
        'shot': []
    }
    
    # Timecode state tracking
    timecode_buffer = []  # Letzte erkannten Timecodes
    timecode_sequence_locked = False  # Flag: 4 gleiche erkannt -> extrapoliere
    base_timecode = None  # Startpunkt für Extrapolation
    base_frame_num = None  # Frame-Nummer des base_timecode
    fps_estimate = None  # Geschätzte FPS aus Sequenz
    timecode_validation_log = []  # Liste von (erkannt, extrapoliert) Paaren zum Validieren
    timecode_accuracy = 0.0  # Genauigkeitsquote
    
    try:
        bbox_detector = SlateDetector(use_segmentation=False)
        segment_detector = SlateDetector(use_segmentation=True)
        slate_detector_ok = True
    except Exception as e:
        slate_detector_ok = False

    start_frame = None
    last_qr_data = None
    segments = []
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    found_bright_frame = False
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        frame_count += 1
        
        if slate_detector_ok:
            if segment_detector.has_slate(frame):
                first_slate_detected = frame_count
                slate_polygon = segment_detector.get_slate_polygon(frame)
                
                if slate_polygon is not None:
                    slate_bbox = slate_polygon.astype(np.float32)
                    # Berechne Bounding Box aus Polygon (min/max Koordinaten)
                    x1 = int(np.min(slate_bbox[:, 0]))
                    y1 = int(np.min(slate_bbox[:, 1]))
                    x2 = int(np.max(slate_bbox[:, 0]))
                    y2 = int(np.max(slate_bbox[:, 1]))
                    h, w = frame.shape[:2]
                    x1, y1 = max(0, x1), max(0, y1)
                    x2, y2 = min(w, x2), min(h, y2)
                    slate_roi = frame[y1:y2, x1:x2]
                else:
                    print("Konnte Polygon nicht extrahieren")
                    continue

                if slate_roi.size > 0 and found_bright_frame == False:
                    decoded_qr = qreader.detect_and_decode(slate_roi)
                    if decoded_qr and decoded_qr[0] :
                        current_qr = decoded_qr[0]
                        if last_qr_data is None:
                            # Erster QR-Code im Video
                            start_frame = frame_count
                            last_qr_data = current_qr
                            print(f"Erster QR-Code erkannt: {current_qr}")
                            slate_start_frame = frame_count
                        elif current_qr != last_qr_data:
                            # QR-Code-Wechsel erkannt, Segment abschließen
                            end_frame = frame_count - 1
                            print(f"Neuer QR-Code erkannt bei frame {frame_count} erkannt: {current_qr}")
                            create_video_with_metadata(video_path, last_qr_data, bright_frame, start_frame, end_frame)
                            # Neues Segment beginnen
                            start_frame = frame_count
                            last_qr_data = current_qr
                            slate_start_frame = frame_count
                            found_bright_frame = False
                            # Reset Timecode-Variablen für neues Segment
                            timecode_buffer = []
                            timecode_sequence_locked = False
                            base_timecode = None
                            base_frame_num = None
                            fps_estimate = None
                            timecode_validation_log = []
                            timecode_accuracy = 0.0
                            # Reset Value-Buffer
                            value_buffer['roll'] = []
                            value_buffer['cam'] = []
                            value_buffer['scene'] = []
                            value_buffer['take'] = []
                            value_buffer['shot'] = []
                        quad_xy = qreader.detect(image=slate_roi)
                        if quad_xy and len(quad_xy) > 0:
                            # Konvertiere relative Koordinaten zurück zu absoluten Frame-Koordinaten
                            polygon = quad_xy[0]['quad_xy']
                            polygon[:, 0] += x1  # X-Offset hinzufügen
                            polygon[:, 1] += y1  # Y-Offset hinzufügen
                    else:
                        timecode = ""
                        reader = easyocr.Reader(['en'])

                        # ROI-Definitionen: (oben links, oben rechts, unten rechts, unten links)
                        roi_defs = {
                            'timecode': np.array([
                                [0.1, 0.2], [0.9, 0.2], [0.9, 0.5], [0.1, 0.5]
                            ], dtype=np.float32),
                            'roll': np.array([
                                [0.1, 0.4], [0.4, 0.4], [0.4, 0.6], [0.1, 0.6]
                            ], dtype=np.float32),
                            'cam': np.array([
                                [0.0, 0.4], [0.12, 0.4], [0.12, 0.6], [0.0, 0.6]
                            ], dtype=np.float32),
                            'scene': np.array([
                                [0.0, 0.55], [0.2, 0.55], [0.2, 0.85], [0.0, 0.85]
                            ], dtype=np.float32),
                            'take': np.array([
                                [0.2, 0.55], [0.4, 0.55], [0.4, 0.85], [0.2, 0.85]
                            ], dtype=np.float32),
                            'shot': np.array([
                                [0.0, 0.8], [0.4, 0.80], [0.4, 1.05], [0.0, 1.05]
                            ], dtype=np.float32)
                        }

                        normalized_quad = np.array([
                            [0, 0],
                            [1, 0],
                            [1, 1],
                            [0, 1]
                        ], dtype=np.float32)
                        H, _ = cv2.findHomography(normalized_quad, slate_polygon)

                        roi_polygons = {}
                        roi_images = {}
                        h, w = frame.shape[:2]
                        grey = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

                        _, high_contrast = cv2.threshold(grey, 128, 255, cv2.THRESH_BINARY)
                        blurred = cv2.GaussianBlur(high_contrast, (5, 5), 0)
                        high_contrast = cv2.cvtColor(blurred, cv2.COLOR_GRAY2BGR)
                        
                        for key, roi_norm in roi_defs.items():
                            roi_norm_reshaped = roi_norm.reshape(-1, 1, 2).astype(np.float32)
                            roi_transformed = cv2.perspectiveTransform(roi_norm_reshaped, H)
                            roi_polygon = roi_transformed.reshape(-1, 2)
                            roi_polygons[key] = roi_polygon
                            x1 = int(np.min(roi_polygon[:, 0]))
                            y1 = int(np.min(roi_polygon[:, 1]))
                            x2 = int(np.max(roi_polygon[:, 0]))
                            y2 = int(np.max(roi_polygon[:, 1]))
                            x1 = max(0, x1)
                            y1 = max(0, y1)
                            x2 = min(w, x2)
                            y2 = min(h, y2)
                            roi_images[key] = high_contrast[y1:y2, x1:x2]


                        if roi_images['timecode'].size > 0:
                            result = reader.readtext(roi_images['timecode'])
                            if result:
                                timecode_raw = result[0][1]
                                timecode_digits = re.sub(r'\D', '', timecode_raw)
                                if len(timecode_digits) == 8:
                                    detected_timecode = f"{timecode_digits[0:2]}:{timecode_digits[2:4]}:{timecode_digits[4:6]}:{timecode_digits[6:8]}"
                                    
                                    # Helper-Funktionen für Timecode-Konvertierung
                                    def timecode_to_frames(tc_str, fps=24):
                                        h, m, s, f = map(int, tc_str.split(':'))
                                        return ((h * 3600 + m * 60 + s) * fps) + f
                                    
                                    def frames_to_timecode(frames, fps=24):
                                        h = frames // (3600 * fps)
                                        m = (frames % (3600 * fps)) // (60 * fps)
                                        s = (frames % (60 * fps)) // fps
                                        f = frames % fps
                                        return f"{h:02d}:{m:02d}:{s:02d}:{f:02d}"
                                    
                                    # Füge zu Buffer hinzu
                                    timecode_buffer.append(detected_timecode)
                                    if len(timecode_buffer) > 3:
                                        timecode_buffer.pop(0)
                                    # Prüfe auf 3 aufeinanderfolgende gleiche Timecodes
                                    if not timecode_sequence_locked and len(timecode_buffer) >= 3:
                                        last_3 = timecode_buffer[-3:]
                                        # Prüfe auf fortlaufende Timecodes (Frames jeweils +1)
                                        fps_candidates = [24, 25, 30, 50, 60]
                                        for fps_cand in fps_candidates:
                                            frames_list = [timecode_to_frames(tc, fps_cand) for tc in last_3]
                                            diffs = [frames_list[i+1] - frames_list[i] for i in range(len(frames_list)-1)]
                                            if all(d == 1 for d in diffs):
                                                timecode_sequence_locked = True
                                                base_timecode = last_3[0]
                                                base_frame_num = frame_count - 2  # Frame der 1. Erkennung (bei 3er-Sequenz: -2)
                                                fps_estimate = fps_cand
                                                print(f"✓ Timecode-Sequenz gesperrt: {base_timecode} @ Frame {base_frame_num} (FPS: {fps_estimate})")
                                                timecode_validation_log = []  # Reset Validierung
                                                break
                                    
                                    # Wenn Sequenz gesperrt: vergleiche erkannt vs. extrapoliert
                                    if timecode_sequence_locked and base_timecode and fps_estimate:
                                        # Berechne erwarteten Timecode für diesen Frame
                                        frames_since_base = frame_count - base_frame_num
                                        base_frames = timecode_to_frames(base_timecode, fps_estimate)
                                        expected_frames = base_frames + frames_since_base
                                        expected_timecode = frames_to_timecode(expected_frames, fps_estimate)
                                        
                                        # Vergleiche erkannt vs. erwartet
                                        is_correct = (detected_timecode == expected_timecode)
                                        timecode_validation_log.append((detected_timecode, expected_timecode, is_correct))
                                        
                                        if is_correct:
                                            print(f"✓ Timecode korrekt: {detected_timecode}")
                                        else:
                                            print(f"✗ Timecode falsch - erkannt: {detected_timecode}, erwartet: {expected_timecode}")
                                    else:
                                        if not timecode_sequence_locked:
                                            print(f"⧗ Timecode-Buffer wird gefüllt ({len(timecode_buffer)}/3): {detected_timecode}")
                                else:
                                    print(f"✗ Timecode erkannt, aber nicht 8 Ziffern: {timecode_digits}")
                            else:
                                print("✗ Kein Timecode im timecode_roi erkannt.")

                            
                        # ROLL
                        if roi_images['roll'].size > 0:
                            result = reader.readtext(roi_images['roll'])
                            if result:
                                roll_raw = result[0][1]
                                roll_digits = re.sub(r'\D', '', roll_raw)
                                if len(roll_digits) == 3:
                                    if found_bright_frame and roll_digits:
                                        value_buffer['roll'].append(roll_digits)
                                    print(f"Erkannter Roll: {roll_digits}")
                                else:
                                    print(f"Roll erkannt, aber nicht 3 Ziffern: {roll_raw}")
                            else:
                                print("Kein Roll im roll_roi erkannt.")
                        else:
                            print("roll_roi ist leer.")

                        # CAM
                        if roi_images['cam'].size > 0:
                            result = reader.readtext(roi_images['cam'])
                            if result:
                                cam_raw = result[0][1]
                                cam_letters = re.sub(r'[^A-Z]', '', cam_raw.upper())
                                if len(cam_letters) == 1:
                                    if found_bright_frame and cam_letters:
                                        value_buffer['cam'].append(cam_letters)
                                    print(f"Erkannte Cam: {cam_letters}")
                                else:
                                    print(f"Cam erkannt, aber nicht genau 1 Buchstabe: {cam_raw}")
                            else:
                                print("Keine Cam im cam_roi erkannt.")
                        else:
                            print("cam_roi ist leer.")

                        # SCENE
                        if roi_images['scene'].size > 0:
                            result = reader.readtext(roi_images['scene'])
                            if result:
                                scene_raw = result[0][1]
                                scene_raw = re.sub(r'\D', '', result[0][1])
                                if len(scene_raw) <= 2:
                                    if found_bright_frame and scene_raw:
                                        value_buffer['scene'].append(scene_raw)
                                    print(f"Erkannte Scene: {scene_raw}")
                            else:
                                print("Keine Scene im scene_roi erkannt.")
                        else:
                            print("scene_roi ist leer.")

                        # TAKE
                        if roi_images['take'].size > 0:
                            result = reader.readtext(roi_images['take'])
                            if result:
                                take_raw = result[0][1]
                                take_raw = re.sub(r'\D', '', result[0][1])
                                if len(take_raw) <= 2:
                                    if found_bright_frame and take_raw:
                                        value_buffer['take'].append(take_raw)
                                    print(f"Erkannte Take: {take_raw}")
                            else:
                                print("Kein Take im take_roi erkannt.")
                        else:
                            print("take_roi ist leer.")

                        # SHOT
                        if roi_images['shot'].size > 0:
                            result = reader.readtext(roi_images['shot'])
                            if result:
                                shot_raw = result[0][1]
                                shot_raw = re.sub(r'[^A-Z0-9]', '', result[0][1].upper())
                                if len(shot_raw) <= 4:
                                    if found_bright_frame and shot_raw:
                                        value_buffer['shot'].append(shot_raw)
                                    print(f"Erkannter Shot: {shot_raw}")
                            else:
                                print("Kein Shot im shot_roi erkannt.")
                        else:
                            print("shot_roi ist leer.")
                            
                        
                        
                    # Bright-frame Detektion in flash_roi (obere linke Ecke der Slate mit Homography)
                    if slate_polygon is not None and found_bright_frame == False:
                        try:
                            # Normalisiertes Rechteck für die flash_roi (20% oben-links)
                            # Koordinaten in normalisierten Slate-Koordinaten (0-1)
                            flash_roi_norm = np.array([
                                [0.0, 0.2],           # Oben-links
                                [0.15, 0.2],           # Oben-rechts (20% Breite)
                                [0.15, 0.4],           # Unten-rechts
                                [0.0, 0.4]            # Unten-links (20% Höhe)
                            ], dtype=np.float32)
                            
                            # Berechne Homography-Matrix von normalisierten zu Polygon-Koordinaten
                            normalized_quad = np.array([
                                [0, 0],
                                [1, 0],
                                [1, 1],
                                [0, 1]
                            ], dtype=np.float32)
                            
                            H, _ = cv2.findHomography(normalized_quad, slate_polygon)
                            
                            # Transformiere flash_roi_norm mit Homography
                            flash_roi_norm_reshaped = flash_roi_norm.reshape(-1, 1, 2).astype(np.float32)
                            flash_roi_transformed = cv2.perspectiveTransform(flash_roi_norm_reshaped, H)
                            flash_roi_polygon = flash_roi_transformed.reshape(-1, 2)
                            
                            # Berechne Bounding Box der transformierten flash_roi
                            flash_x1 = int(np.min(flash_roi_polygon[:, 0]))
                            flash_y1 = int(np.min(flash_roi_polygon[:, 1]))
                            flash_x2 = int(np.max(flash_roi_polygon[:, 0]))
                            flash_y2 = int(np.max(flash_roi_polygon[:, 1]))
                            
                            # Stelle sicher, dass die Koordinaten im Bild liegen
                            h, w = frame.shape[:2]
                            flash_x1 = max(0, flash_x1)
                            flash_y1 = max(0, flash_y1)
                            flash_x2 = min(w, flash_x2)
                            flash_y2 = min(h, flash_y2)
                            
                            flash_roi = frame[flash_y1:flash_y2, flash_x1:flash_x2]
                            
                            if flash_roi.size > 0:
                                # Suche nach hellen Pixeln
                                bright_mask = np.all(flash_roi > 200, axis=2).astype(np.uint8) * 255
                                
                                # Konturen der hellen Bereiche finden
                                contours, _ = cv2.findContours(bright_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
                                
                                roi_area = bright_mask.size
                                min_bright_area = max(1, int(roi_area * 0.01))  # 1% der ROI-Fläche
                                
                                max_area = 0
                                max_cnt = None
                                for cnt in contours:
                                    area = cv2.contourArea(cnt)
                                    if area >= min_bright_area and area > max_area:
                                        max_area = area
                                        max_cnt = cnt
                                
                                if max_cnt is not None:
                                    # Mittelpunkt der Kontur berechnen
                                    M = cv2.moments(max_cnt)
                                    if M["m00"] != 0:
                                        cx = int(M["m10"] / M["m00"]) + flash_x1
                                        cy = int(M["m01"] / M["m00"]) + flash_y1
                                        bright_area = int(np.sqrt(max_area / np.pi)) * 3
                                        print("Bright frame Detected in flash_roi")
                                        bright_frame = frame_count
                                        found_bright_frame = True
                                        
                                        # Berechne häufigste Werte
                                        from collections import Counter
                                        print("\n=== Häufigste erkannte Werte ===")
                                        for key in ['roll', 'cam', 'scene', 'take', 'shot']:
                                            values = value_buffer[key]
                                            if values:
                                                most_common = Counter(values).most_common(1)[0][0]
                                                count = Counter(values).most_common(1)[0][1]
                                                print(f"{key.upper()}: {most_common} (Häufigkeit: {count})")
                                            else:
                                                print(f"{key.upper()}: Nicht erkannt")
                                        print("=" * 35 + "\n")
                                        
                                        # Timecode-Accuracy berechnen und entsprechend speichern
                                        if timecode_sequence_locked and timecode_validation_log:
                                            correct_count = sum(1 for _, _, is_correct in timecode_validation_log if is_correct)
                                            total_count = len(timecode_validation_log)
                                            timecode_accuracy = (correct_count / total_count) * 100 if total_count > 0 else 0.0
                                            
                                            print(f"\n=== Timecode-Validierung ===")
                                            print(f"Korrekt: {correct_count}/{total_count} ({timecode_accuracy:.1f}%)")
                                            
                                            # Berechne finalen Timecode durch Extrapolation
                                            def timecode_to_frames(tc_str, fps=24):
                                                h, m, s, f = map(int, tc_str.split(':'))
                                                return ((h * 3600 + m * 60 + s) * fps) + f
                                            
                                            def frames_to_timecode(frames, fps=24):
                                                h = frames // (3600 * fps)
                                                m = (frames % (3600 * fps)) // (60 * fps)
                                                s = (frames % (60 * fps)) // fps
                                                f = frames % fps
                                                return f"{h:02d}:{m:02d}:{s:02d}:{f:02d}"
                                            
                                            frames_since_base = frame_count - base_frame_num
                                            base_frames = timecode_to_frames(base_timecode, fps_estimate)
                                            final_frames = base_frames + frames_since_base
                                            final_timecode = frames_to_timecode(final_frames, fps_estimate)
                                            
                                            print(f"Finaler Timecode: {final_timecode}")
                                            print("=" * 35 + "\n")
                                            
                                            # Auto-save bei >70% Accuracy, sonst Benutzer-Abfrage
                                            if timecode_accuracy >= 70.0:
                                                bright_frame = frame_count  # Auto-save
                                                print(f"✓ Timecode-Accuracy >= 70% ({timecode_accuracy:.1f}%) → AUTOMATISCH GESPEICHERT")
                                            else:
                                                print(f"⚠ Timecode-Accuracy < 70% ({timecode_accuracy:.1f}%)")
                                                try:
                                                    user_input = input(f"Soll Frame {frame_count} mit Timecode {final_timecode} gespeichert werden? (y/n): ").strip().lower()
                                                    if user_input == 'y':
                                                        bright_frame = frame_count
                                                        print(f"✓ Frame {frame_count} MANUELL GESPEICHERT")
                                                    else:
                                                        print(f"✗ Frame {frame_count} NICHT gespeichert")
                                                except Exception as e:
                                                    print(f"Fehler bei Benutzer-Eingabe: {e}")
                                            
                                            timecode = final_timecode
                                        else:
                                            timecode = "00:00:00:00"
                                            print("Kein stabiler Timecode verfügbar, verwende Fallback")
                        except Exception as e:
                            print(f"Fehler bei flash_roi Homography: {e}")
            else:
                slate_polygon = None
                polygon = None
                found_bright_frame = False
                # Reset Value-Buffer und Timecode-Variablen wenn Klappe verloren geht
                value_buffer['roll'] = []
                value_buffer['cam'] = []
                value_buffer['scene'] = []
                value_buffer['take'] = []
                value_buffer['shot'] = []
                timecode_buffer = []
                timecode_sequence_locked = False
                base_timecode = None
                base_frame_num = None
                fps_estimate = None
                timecode_validation_log = []
                timecode_accuracy = 0.0
                print(f"Frame {frame_count}: No slate detected.")
        if show_frame:
            frame_with_rect = frame.copy()
            if slate_detector_ok:
                # Draw slate detection polygon
                if slate_polygon is not None:
                    polygon_int = slate_polygon.reshape(-1, 1, 2).astype(np.int32)
                    cv2.polylines(frame_with_rect, [polygon_int], isClosed=True,
                        color=(0, 0, 255), thickness=2)
                    cv2.putText(frame_with_rect, "ARC-SLATE",
                        tuple(slate_polygon[0].astype(int) + np.array([10, -20])),
                        cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)

                    # Draw flash_roi polygon (nur wenn im aktuellen Frame erkannt)
                    if 'flash_roi_polygon' in locals() and flash_roi_polygon is not None:
                        flash_roi_int = flash_roi_polygon.reshape(-1, 1, 2).astype(np.int32)
                        if found_bright_frame:
                            cv2.polylines(frame_with_rect, [flash_roi_int], isClosed=True,
                                color=(0, 255, 0), thickness=2)
                            cv2.putText(frame_with_rect, "flash_roi",
                                tuple(flash_roi_polygon[0].astype(int) + np.array([5, -5])),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
                        else:
                            cv2.polylines(frame_with_rect, [flash_roi_int], isClosed=True,
                                color=(0, 0, 255), thickness=2)
                            cv2.putText(frame_with_rect, "flash_roi",
                                tuple(flash_roi_polygon[0].astype(int) + np.array([5, -5])),
                                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)

                    # Draw all ROI polygons (timecode, roll, cam, scene, take, shot)
                    roi_colors = {
                        'timecode': (0, 255, 255),
                        'roll': (0, 255, 255),
                        'cam': (255, 128, 0),
                        'scene': (128, 0, 255),
                        'take': (0, 128, 255),
                        'shot': (255, 0, 128)
                    }
                    roi_labels = {
                        'timecode': 'timecode_roi',
                        'roll': 'roll_roi',
                        'cam': 'cam_roi',
                        'scene': 'scene_roi',
                        'take': 'take_roi',
                        'shot': 'shot_roi'
                    }
                    if 'roi_polygons' in locals():
                        for key in roi_labels.keys():
                            poly = roi_polygons.get(key)
                            if poly is not None:
                                poly_int = poly.reshape(-1, 1, 2).astype(np.int32)
                                cv2.polylines(frame_with_rect, [poly_int], isClosed=True,
                                    color=roi_colors[key], thickness=2)
                                cv2.putText(frame_with_rect, roi_labels[key],
                                    tuple(poly[0].astype(int) + np.array([5, -5])),
                                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, roi_colors[key], 2)
                # Draw QR-Code polygon nur wenn im aktuellen Frame erkannt
            if isinstance(polygon, np.ndarray) and polygon.shape == (4, 2) and decoded_qr:
                polygon_int = polygon.reshape(-1, 1, 2).astype(np.int32)
                cv2.polylines(frame_with_rect, [polygon_int], isClosed=True,
                    color=(255, 0, 0), thickness=2)
                cv2.putText(frame_with_rect, "QR-Code",
                    tuple(polygon[0].astype(int)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)

            # Fallback: Nur das Frame anzeigen
            cv2.imshow('Frame with ROI', frame_with_rect)
            cv2.waitKey(1)

    # Letztes Segment abschließen, falls vorhanden
    if last_qr_data is not None and start_frame is not None:
        end_frame = total_frames - 1
        print(f"Letztes Segment: {last_qr_data} von Frame {start_frame} bis {end_frame}")
        create_video_with_metadata(video_path, last_qr_data, None, start_frame, end_frame)

    cap.release()

# --- Hauptablauf ---
import_folder = os.path.join(project_location, "01_IMPORT")
video_files = sorted(glob.glob(os.path.join(import_folder, "*.mov"))) + \
              sorted(glob.glob(os.path.join(import_folder, "*.MOV")))

if not video_files:
    print("Error: No video files found in 01_IMPORT folder.")
    sys.exit(1)

for video_path in video_files:
    print(f"\n--- Verarbeitung von: {video_path} ---")
    process_video(video_path, True)
