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

# ==================== GLOBAL STATE ====================
state_slate_detected = False
state_qr_detected = False
state_bright_frame_detected = False
state_slate_polygon = None
state_qr_data = None
state_bright_frame_num = None
state_start_frame = None
state_last_qr_data = None
state_used_takes = []
state_json_meta = None
state_slate_detection_buffer = []  # Buffer for 3-frame slate detection stability
state_sequential_take_counter = 0  # Counter for sequential takes from JSON

# Previous segment data (waiting for validation by next bright frame)
state_previous_segment = {
    'start_frame': None,
    'end_frame': None,
    'bright_frame_num': None,
    'qr_code': None,
    'validated': False  # True if bright frame was detected
}

# ==================== HELPER FUNCTIONS ====================

def reset_state():
    """Reset all state variables"""
    global state_slate_detected, state_qr_detected, state_bright_frame_detected
    global state_slate_polygon, state_qr_data, state_bright_frame_num
    state_slate_detected = False
    state_qr_detected = False
    state_bright_frame_detected = False
    state_slate_polygon = None
    state_qr_data = None
    state_bright_frame_num = None

def load_json_metadata():
    """Load JSON metadata from project folder"""
    global state_json_meta
    try:
        json_folder = os.path.join(project_location, "02_JSONFILE")
        json_files = sorted(glob.glob(os.path.join(json_folder, "*.json")))
        if json_files:
            with open(json_files[0], 'r') as f:
                state_json_meta = json.load(f)
                print(f"JSON metadata loaded: {json_files[0]}")
                return True
    except Exception as e:
        print(f"Error loading JSON metadata: {e}")
    return False

# ==================== DETECTION FUNCTIONS ====================

def detect_slate(frame, detector, frame_count, video_path):
    """Detect slate/clapperboard in frame with 3-frame stability buffer"""
    global state_slate_detected, state_slate_polygon, state_start_frame, state_slate_detection_buffer
    global state_bright_frame_detected, state_bright_frame_num, state_last_qr_data, state_previous_segment
    
    # Check if slate is present in current frame
    slate_present = False
    polygon = None
    
    if detector.has_slate(frame):
        polygon = detector.get_slate_polygon(frame)
        if polygon is not None:
            slate_present = True
    
    # Update detection buffer (keep last 3 frames)
    state_slate_detection_buffer.append(slate_present)
    if len(state_slate_detection_buffer) > 3:
        state_slate_detection_buffer.pop(0)
    
    # Need 3 consecutive frames to change state
    if len(state_slate_detection_buffer) == 3:
        # All 3 frames detected slate -> NEW slate appears
        if all(state_slate_detection_buffer) and not state_slate_detected:
            new_start_frame = frame_count - 2  # Go back 2 frames to actual slate start
            
            # If there's a previous validated segment, set its end_frame NOW (new_start - 1)
            if (state_previous_segment['validated'] and 
                state_previous_segment['start_frame'] is not None and
                state_previous_segment['end_frame'] is None):
                state_previous_segment['end_frame'] = new_start_frame - 1
                print(f"Frame {frame_count}: New slate detected - setting end_frame for previous segment to {state_previous_segment['end_frame']}")
            
            # Set new slate as detected
            state_slate_detected = True
            state_start_frame = new_start_frame
            state_bright_frame_detected = False
            state_bright_frame_num = None
            state_last_qr_data = None
            print(f"Frame {frame_count}: Slate detected - start_frame={state_start_frame}")
            
        # All 3 frames without slate -> slate disappears (just log, don't set end_frame)
        elif not any(state_slate_detection_buffer) and state_slate_detected:
            print(f"Frame {frame_count}: Slate lost")
            
            # Save current segment data (WITHOUT end_frame - will be set when next slate appears)
            state_previous_segment = {
                'start_frame': state_start_frame,
                'end_frame': None,  # Will be set when next slate appears
                'bright_frame_num': state_bright_frame_num,
                'qr_code': state_last_qr_data,
                'validated': state_bright_frame_detected
            }
            
            if state_bright_frame_detected:
                print(f"  → Segment validated (bright frame at {state_bright_frame_num}) - waiting for next slate to set end_frame")
            else:
                print(f"  → Segment NOT validated (no bright frame) - will be discarded")
            
            # Reset current state
            state_slate_detected = False
            state_start_frame = None
            state_bright_frame_num = None
            state_bright_frame_detected = False
            state_last_qr_data = None
    
    # Update polygon if slate is currently present
    if slate_present and polygon is not None:
        state_slate_polygon = polygon
        return True, polygon
    elif not slate_present:
        state_slate_polygon = None
    
    return state_slate_detected, state_slate_polygon

def extract_slate_roi(frame, slate_polygon):
    """Extract ROI from slate polygon"""
    slate_bbox = slate_polygon.astype(np.float32)
    x1 = int(np.min(slate_bbox[:, 0]))
    y1 = int(np.min(slate_bbox[:, 1]))
    x2 = int(np.max(slate_bbox[:, 0]))
    y2 = int(np.max(slate_bbox[:, 1]))
    h, w = frame.shape[:2]
    x1, y1 = max(0, x1), max(0, y1)
    x2, y2 = min(w, x2), min(h, y2)
    return frame[y1:y2, x1:x2], (x1, y1)

def detect_qr_code(slate_roi, qreader, offset_x, offset_y):
    """Detect and decode QR code in slate ROI"""
    global state_qr_detected, state_qr_data
    
    if slate_roi.size == 0:
        state_qr_detected = False
        state_qr_data = None
        return None, None
    
    decoded_qr = qreader.detect_and_decode(slate_roi)
    if decoded_qr and decoded_qr[0]:
        state_qr_detected = True
        state_qr_data = decoded_qr[0]
        
        quad_xy = qreader.detect(image=slate_roi)
        polygon = None
        if quad_xy and len(quad_xy) > 0:
            polygon = quad_xy[0]['quad_xy']
            polygon[:, 0] += offset_x
            polygon[:, 1] += offset_y
        
        return decoded_qr[0], polygon
    
    state_qr_detected = False
    state_qr_data = None
    return None, None

def detect_bright_frame(frame, slate_polygon, frame_count, video_path):
    """Detect bright frame/flash in slate region - validates previous segment when detected"""
    global state_bright_frame_detected, state_bright_frame_num, state_previous_segment
    
    if slate_polygon is None:
        return False, None
    
    try:
        flash_roi_norm = np.array([
            [0.0, 0.2],
            [0.2, 0.2],
            [0.2, 0.5],
            [0.0, 0.5]
        ], dtype=np.float32)
        
        normalized_quad = np.array([
            [0, 0],
            [1, 0],
            [1, 1],
            [0, 1]
        ], dtype=np.float32)
        
        H, _ = cv2.findHomography(normalized_quad, slate_polygon)
        flash_roi_norm_reshaped = flash_roi_norm.reshape(-1, 1, 2).astype(np.float32)
        flash_roi_transformed = cv2.perspectiveTransform(flash_roi_norm_reshaped, H)
        flash_roi_polygon = flash_roi_transformed.reshape(-1, 2)
        
        flash_x1 = int(np.min(flash_roi_polygon[:, 0]))
        flash_y1 = int(np.min(flash_roi_polygon[:, 1]))
        flash_x2 = int(np.max(flash_roi_polygon[:, 0]))
        flash_y2 = int(np.max(flash_roi_polygon[:, 1]))
        
        h, w = frame.shape[:2]
        flash_x1 = max(0, flash_x1)
        flash_y1 = max(0, flash_y1)
        flash_x2 = min(w, flash_x2)
        flash_y2 = min(h, flash_y2)
        
        flash_roi = frame[flash_y1:flash_y2, flash_x1:flash_x2]
        
        if flash_roi.size > 0:
            bright_mask = np.all(flash_roi > 200, axis=2).astype(np.uint8) * 255
            contours, _ = cv2.findContours(bright_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
            
            roi_area = bright_mask.size
            min_bright_area = max(1, int(roi_area * 0.01))
            
            max_area = 0
            max_cnt = None
            for cnt in contours:
                area = cv2.contourArea(cnt)
                if area >= min_bright_area and area > max_area:
                    max_area = area
                    max_cnt = cnt
            
            if max_cnt is not None:
                M = cv2.moments(max_cnt)
                if M["m00"] != 0:
                    # Bright frame detected!
                    state_bright_frame_detected = True
                    state_bright_frame_num = frame_count
                    print(f"Bright frame detected at frame {frame_count}")
                    
                    # Check if there's a validated previous segment with end_frame set
                    if (state_previous_segment['validated'] and 
                        state_previous_segment['start_frame'] is not None and
                        state_previous_segment['end_frame'] is not None):
                        print(f"  → Creating PREVIOUS segment: frames {state_previous_segment['start_frame']}-{state_previous_segment['end_frame']}")
                        finalize_slate_segment(
                            video_path,
                            state_previous_segment['start_frame'],
                            state_previous_segment['end_frame'],
                            state_previous_segment['bright_frame_num'],
                            state_previous_segment['qr_code']
                        )
                        # Clear previous segment after creation
                        state_previous_segment = {
                            'start_frame': None,
                            'end_frame': None,
                            'bright_frame_num': None,
                            'qr_code': None,
                            'validated': False
                        }
                    
                    return True, flash_roi_polygon
        
        return False, None
    except Exception as e:
        print(f"Error detecting bright frame: {e}")
        return False, None

# ==================== VIDEO PROCESSING FUNCTIONS ====================

def finalize_slate_segment(video_path, start_frame, end_frame, bright_frame_num, qr_code):
    """Finalize slate segment - only create video if bright frame was detected"""
    global state_json_meta, state_used_takes
    
    if bright_frame_num is None:
        print(f"Slate segment from frame {start_frame} to {end_frame} - NO bright frame detected, skipping video creation")
        return
    
    print(f"Slate segment from frame {start_frame} to {end_frame} with bright frame at {bright_frame_num}")
    print("Creating video segment")
    
    create_video_with_metadata(
        video_path,
        qr_code,
        bright_frame_num,
        start_frame,
        end_frame,
        state_json_meta,
        state_used_takes
    )

def store_qr_code(qr_data):
    """Store QR code data for current segment"""
    global state_last_qr_data
    
    if state_last_qr_data != qr_data:
        state_last_qr_data = qr_data
        print(f"QR code stored: {qr_data}")

def visualize_frame(frame, slate_polygon, qr_polygon, flash_roi_polygon, bright_detected):
    """Visualize detection results on frame"""
    frame_with_rect = frame.copy()
    
    if slate_polygon is not None:
        polygon_int = slate_polygon.reshape(-1, 1, 2).astype(np.int32)
        cv2.polylines(frame_with_rect, [polygon_int], isClosed=True, color=(0, 0, 255), thickness=2)
        cv2.putText(frame_with_rect, "ARC-SLATE",
            tuple(slate_polygon[0].astype(int) + np.array([10, -20])),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 255), 2)
    
    if flash_roi_polygon is not None:
        flash_roi_int = flash_roi_polygon.reshape(-1, 1, 2).astype(np.int32)
        color = (0, 255, 0) if bright_detected else (0, 0, 255)
        cv2.polylines(frame_with_rect, [flash_roi_int], isClosed=True, color=color, thickness=2)
        cv2.putText(frame_with_rect, "flash_roi",
            tuple(flash_roi_polygon[0].astype(int) + np.array([5, -5])),
            cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2)
    
    if qr_polygon is not None:
        polygon_int = qr_polygon.reshape(-1, 1, 2).astype(np.int32)
        cv2.polylines(frame_with_rect, [polygon_int], isClosed=True, color=(255, 0, 0), thickness=2)
        cv2.putText(frame_with_rect, "QR-Code",
            tuple(qr_polygon[0].astype(int)),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 0, 0), 2)
    
    cv2.imshow('Frame with ROI', frame_with_rect)
    cv2.waitKey(1)

# ==================== TIMECODE AND VIDEO CREATION ====================

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

def get_next_take_from_json(json_meta, used_takes=None):
    """Get next available take from JSON chronologically"""
    global state_sequential_take_counter
    
    if used_takes is None:
        used_takes = []
    
    current_index = 0
    for shot in json_meta.get("shotlist", []):
        for take in shot.get("takes", []):
            if current_index == state_sequential_take_counter:
                state_sequential_take_counter += 1
                return {
                    'scene': str(shot.get("scene")),
                    'shot': str(shot.get("shot")),
                    'roll': take.get("roll"),
                    'take': str(take.get("take")),
                    'timecode': take.get("timecode", "00:00:00:00"),
                    'framerate': shot.get("framerate", 24)
                }
            current_index += 1
    return None

def get_take_by_qr_code(json_meta, qr_data):
    """Get take info from JSON matching QR code data"""
    try:
        parts = qr_data.split('_')
        if len(parts) >= 6:
            qr_scene = parts[3]
            qr_shot = parts[4]
            qr_take = parts[5]
            
            for shot in json_meta.get("shotlist", []):
                if str(shot.get("scene")) == qr_scene and str(shot.get("shot")) == qr_shot:
                    for take in shot.get("takes", []):
                        if str(take.get("take")) == qr_take:
                            return {
                                'scene': str(shot.get("scene")),
                                'shot': str(shot.get("shot")),
                                'roll': take.get("roll"),
                                'take': str(take.get("take")),
                                'timecode': take.get("timecode", "00:00:00:00"),
                                'framerate': shot.get("framerate", 24)
                            }
    except Exception as e:
        print(f"Error parsing QR code: {e}")
    return None

def sync_take_counter_with_qr(json_meta, qr_data):
    """Synchronize sequential take counter with QR code position"""
    global state_sequential_take_counter
    
    try:
        parts = qr_data.split('_')
        if len(parts) >= 6:
            qr_scene = parts[3]
            qr_shot = parts[4]
            qr_take = parts[5]
            
            current_index = 0
            for shot in json_meta.get("shotlist", []):
                for take in shot.get("takes", []):
                    if (str(shot.get("scene")) == qr_scene and 
                        str(shot.get("shot")) == qr_shot and 
                        str(take.get("take")) == qr_take):
                        state_sequential_take_counter = current_index + 1
                        print(f"Take counter synchronized with QR code: position {current_index + 1}")
                        return
                    current_index += 1
    except Exception as e:
        print(f"Error synchronizing take counter: {e}")

def create_video_with_metadata(video_path, qr_code_data, sync_frame, start_frame, end_frame, json_meta=None, used_takes=None):
    """Create video with metadata from QR code or JSON"""
    global current_code, current_version, current_roll, current_scene, current_shot, current_take
    
    # Always use JSON data for metadata
    if json_meta is None:
        print("No JSON metadata available")
        return
    
    # If QR code present, try to use it and sync counter
    if qr_code_data:
        take_info = get_take_by_qr_code(json_meta, qr_code_data)
        if take_info:
            # Sync counter with QR code position
            sync_take_counter_with_qr(json_meta, qr_code_data)
        else:
            # QR code doesn't match JSON, use sequential
            take_info = get_next_take_from_json(json_meta, used_takes)
    else:
        # No QR code, use sequential from JSON
        take_info = get_next_take_from_json(json_meta, used_takes)
    
    if take_info is None:
        print("No more takes available in JSON")
        return
    
    # Extract project code: from QR if available, otherwise from JSON
    if qr_code_data:
        parts = qr_code_data.split('_')
        if len(parts) >= 1:
            current_code = parts[0]
        else:
            current_code = json_meta.get("code", "PROJECT")
    else:
        current_code = json_meta.get("code", "PROJECT")
    
    current_version = "AUTO"
    current_roll = take_info['roll']
    current_scene = take_info['scene']
    current_shot = take_info['shot']
    current_take = take_info['take']
    qr_status = 'Yes' if qr_code_data else 'No'
    print(f"Using metadata - Code: {current_code}, Roll: {current_roll}, Scene: {current_scene}, Shot: {current_shot}, Take: {current_take} (QR: {qr_status})")

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
    """Main video processing loop with clean state-based logic"""
    global state_start_frame, state_last_qr_data, state_used_takes, state_bright_frame_detected
    
    # Initialize
    cap = cv2.VideoCapture(video_path)
    frame_count = 0
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    qreader = QReader('l')
    
    # Load JSON metadata
    load_json_metadata()
    
    # Initialize detector
    try:
        detector = SlateDetector(use_segmentation=True)
        print("Slate detector initialized successfully")
    except Exception as e:
        print(f"Error initializing slate detector: {e}")
        cap.release()
        return
    
    # Reset state
    state_start_frame = None
    state_last_qr_data = None
    state_used_takes = []
    state_slate_detection_buffer.clear()
    state_sequential_take_counter = 0
    
    # Temporary variables for visualization
    qr_polygon_viz = None
    flash_roi_polygon_viz = None
    
    print(f"Starting video processing: {total_frames} frames")
    
    # Main loop
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        frame_count += 1
        
        # Reset visualization variables
        qr_polygon_viz = None
        flash_roi_polygon_viz = None
        
        # STEP 1: Detect slate (with 3-frame buffering)
        slate_found, slate_polygon = detect_slate(frame, detector, frame_count, video_path)
        
        # STEP 2-4: Process slate if detected and visible
        if state_slate_detected and slate_found and slate_polygon is not None:
            # STEP 2: Extract slate ROI
            slate_roi, (offset_x, offset_y) = extract_slate_roi(frame, slate_polygon)
            
            # STEP 3: Detect QR code and store it
            qr_data, qr_polygon_viz = detect_qr_code(slate_roi, qreader, offset_x, offset_y)
            
            if qr_data:
                store_qr_code(qr_data)
            
            # STEP 4: Detect bright frame (only if not yet detected in this segment)
            if not state_bright_frame_detected:
                bright_found, flash_roi_polygon_viz = detect_bright_frame(frame, slate_polygon, frame_count, video_path)
        
        # STEP 5: Visualization
        if show_frame:
            visualize_frame(
                frame, 
                state_slate_polygon, 
                qr_polygon_viz,
                flash_roi_polygon_viz,
                state_bright_frame_detected
            )
    
    # Handle last segment: If there's a validated segment without end_frame, use total_frames
    if (state_previous_segment['validated'] and 
        state_previous_segment['start_frame'] is not None and
        state_previous_segment['end_frame'] is None):
        state_previous_segment['end_frame'] = total_frames - 1
        print(f"Setting end_frame for last segment to {state_previous_segment['end_frame']} (end of video)")
        print(f"Creating LAST segment: frames {state_previous_segment['start_frame']}-{state_previous_segment['end_frame']}")
        finalize_slate_segment(
            video_path,
            state_previous_segment['start_frame'],
            state_previous_segment['end_frame'],
            state_previous_segment['bright_frame_num'],
            state_previous_segment['qr_code']
        )
    
    cap.release()
    print(f"Video processing completed: {frame_count} frames processed")

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
