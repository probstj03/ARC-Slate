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

project_location = '/Volumes/JULIAN_SSD/Videos/2025 Schlüsselloch/ARC-SLATE-0005'
# ==================== GLOBAL STATE ====================
write_bboxes_to_video = True  # CONFIGURATION: If True, all bounding boxes shown in visualization will be written to final video
state_slate_detected = False
state_qr_detected = False
state_bright_frame_detected = False
state_slate_polygon = None
state_qr_data = None
state_bright_frame_num = None
state_start_frame = None
state_last_qr_data = None
state_segment_qr_data = None  # QR code valid for entire segment (persists even if lost)
state_used_takes = []
state_json_meta = None
state_slate_detection_buffer = []  # Buffer for 3-frame slate detection stability
state_current_roll = None  # Current roll being processed (e.g., A001, B002)
state_slate_polygon_history = []  # Buffer for polygon stabilization (last 2 valid polygons)

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

def detect_roll_from_video(video_path, json_meta):
    """Detect which roll this video belongs to by checking available rolls in JSON"""
    # Get all unique rolls from JSON
    available_rolls = set()
    for shot in json_meta.get("shotlist", []):
        for take in shot.get("takes", []):
            roll = take.get("roll")
            if roll:
                available_rolls.add(roll)
    
    if not available_rolls:
        print("Warning: No rolls found in JSON")
        return None
    
    # If only one roll, use it
    if len(available_rolls) == 1:
        roll = list(available_rolls)[0]
        print(f"Single roll found in JSON: {roll}")
        return roll
    
    # Multiple rolls: try to extract from filename
    filename = os.path.basename(video_path)
    for roll in available_rolls:
        if roll in filename:
            print(f"Roll detected from filename: {roll}")
            return roll
    
    # Could not detect from filename: ask user
    sorted_rolls = sorted(available_rolls)
    print(f"\nMultiple rolls found in JSON: {', '.join(sorted_rolls)}")
    print(f"Could not detect roll from filename: {filename}")
    
    while True:
        user_input = input(f"Please enter the roll for this video ({', '.join(sorted_rolls)}): ").strip().upper()
        if user_input in available_rolls:
            print(f"Using roll: {user_input}")
            return user_input
        else:
            print(f"Invalid roll. Please choose from: {', '.join(sorted_rolls)}")

# ==================== DETECTION FUNCTIONS ====================

def calculate_polygon_angle(polygon):
    """Calculate the maximum angle of the polygon relative to camera (perspective distortion)"""
    if polygon is None or len(polygon) < 4:
        return 0
    
    # Calculate angles between edges
    edges = []
    for i in range(4):
        p1 = polygon[i]
        p2 = polygon[(i + 1) % 4]
        edge = p2 - p1
        edges.append(edge)
    
    # Calculate angles between consecutive edges
    max_angle = 0
    for i in range(4):
        e1 = edges[i]
        e2 = edges[(i + 1) % 4]
        
        # Calculate angle using dot product
        dot = np.dot(e1, e2)
        norm1 = np.linalg.norm(e1)
        norm2 = np.linalg.norm(e2)
        
        if norm1 > 0 and norm2 > 0:
            cos_angle = dot / (norm1 * norm2)
            cos_angle = np.clip(cos_angle, -1.0, 1.0)
            angle = np.degrees(np.arccos(np.abs(cos_angle)))
            
            # Deviation from 90 degrees (rectangle)
            deviation = abs(90 - angle)
            max_angle = max(max_angle, deviation)
    
    return max_angle

def polygon_changed_significantly(poly1, poly2, threshold=50):
    """Check if polygon changed significantly between frames"""
    if poly1 is None or poly2 is None:
        return True
    
    # Calculate average distance between corresponding points
    distances = [np.linalg.norm(poly1[i] - poly2[i]) for i in range(4)]
    avg_distance = np.mean(distances)
    
    return avg_distance > threshold

def stabilize_polygon(new_polygon, max_angle=45, max_change=50):
    """Stabilize polygon detection using history buffer"""
    global state_slate_polygon_history
    
    if new_polygon is None:
        return None
    
    # Check angle constraint
    angle = calculate_polygon_angle(new_polygon)
    if angle > max_angle:
        # Angle too steep, use previous polygon if available
        if len(state_slate_polygon_history) > 0:
            return state_slate_polygon_history[-1]
        else:
            return None
    
    # Check if polygon changed significantly
    if len(state_slate_polygon_history) > 0:
        last_polygon = state_slate_polygon_history[-1]
        if polygon_changed_significantly(last_polygon, new_polygon, max_change):
            # Significant change detected
            # Check if change is consistent (at least 2 frames)
            if len(state_slate_polygon_history) >= 2:
                second_last = state_slate_polygon_history[-2]
                # If previous polygon was also different, accept new one
                if polygon_changed_significantly(second_last, last_polygon, max_change):
                    # Position is consistently changing, accept new polygon
                    state_slate_polygon_history.append(new_polygon)
                else:
                    # Change not consistent yet, keep previous
                    return last_polygon
            else:
                # Not enough history, keep previous
                return last_polygon
        else:
            # Small change, accept and update history
            state_slate_polygon_history.append(new_polygon)
    else:
        # First polygon, add to history
        state_slate_polygon_history.append(new_polygon)
    
    # Keep only last 3 polygons in history
    if len(state_slate_polygon_history) > 3:
        state_slate_polygon_history.pop(0)
    
    return new_polygon

def detect_slate(frame, detector, frame_count, video_path):
    """Detect slate/clapperboard in frame with 3-frame stability buffer"""
    global state_slate_detected, state_slate_polygon, state_start_frame, state_slate_detection_buffer
    global state_bright_frame_detected, state_bright_frame_num, state_last_qr_data, state_segment_qr_data, state_previous_segment
    
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
            new_start_frame = frame_count  # Use current frame to avoid black frames at start
            
            # DO NOT set end_frame here!
            # The end_frame will be set when the NEW slate gets a bright frame (validated)
            # or at the end of the video if no validated slate follows
            
            # Set new slate as detected
            state_slate_detected = True
            state_start_frame = new_start_frame
            state_bright_frame_detected = False
            state_bright_frame_num = None
            state_last_qr_data = None
            state_segment_qr_data = None  # Reset segment QR at start of new segment
            print(f"Frame {frame_count}: Slate detected - start_frame={state_start_frame}")
            
        # All 3 frames without slate -> slate disappears (just log, don't set end_frame)
        elif not any(state_slate_detection_buffer) and state_slate_detected:
            print(f"Frame {frame_count}: Slate lost")
            
            # CRITICAL: Only save to state_previous_segment if THIS segment was validated!
            # Non-validated segments should NOT overwrite a previous validated segment
            if state_bright_frame_detected:
                # This segment was validated (has bright frame) - save it
                state_previous_segment = {
                    'start_frame': state_start_frame,
                    'end_frame': None,  # Will be set when next validated slate appears
                    'bright_frame_num': state_bright_frame_num,
                    'qr_code': state_segment_qr_data,  # Use segment-wide QR code
                    'validated': True
                }
                print(f"  → Segment validated (bright frame at {state_bright_frame_num}) - waiting for next validated slate to set end_frame")
            else:
                # This segment was NOT validated - just discard it, don't touch state_previous_segment
                print(f"  → Segment NOT validated (no bright frame) - will be discarded")
            
            # Reset current state
            state_slate_detected = False
            state_start_frame = None
            state_bright_frame_num = None
            state_bright_frame_detected = False
            state_last_qr_data = None
            state_segment_qr_data = None  # Reset segment QR when slate is lost
    
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
    global state_bright_frame_detected, state_bright_frame_num, state_previous_segment, state_start_frame
    
    if slate_polygon is None:
        return False, None
    
    try:
        flash_roi_norm = np.array([
            [0.0, 0.0],
            [0.3, 0.0],
            [0.3, 0.5],
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
            # Extract the entire slate region for average brightness calculation
            slate_x1 = int(np.min(slate_polygon[:, 0]))
            slate_y1 = int(np.min(slate_polygon[:, 1]))
            slate_x2 = int(np.max(slate_polygon[:, 0]))
            slate_y2 = int(np.max(slate_polygon[:, 1]))
            
            slate_x1 = max(0, slate_x1)
            slate_y1 = max(0, slate_y1)
            slate_x2 = min(w, slate_x2)
            slate_y2 = min(h, slate_y2)
            
            slate_roi = frame[slate_y1:slate_y2, slate_x1:slate_x2]
            
            # Calculate average brightness of entire slate (grayscale)
            if slate_roi.size > 0:
                gray_slate = cv2.cvtColor(slate_roi, cv2.COLOR_BGR2GRAY)
                avg_slate_brightness = np.mean(gray_slate)
            else:
                avg_slate_brightness = 128  # Fallback
            
            # Dynamic threshold: Flash ROI must be significantly brighter than slate average
            # Use a factor (e.g., 3x) above slate average + minimum offset (e.g., 50)
            brightness_threshold = max(avg_slate_brightness * 2.2, avg_slate_brightness + 30)
            
            # Convert flash ROI to grayscale for comparison
            gray_roi = cv2.cvtColor(flash_roi, cv2.COLOR_BGR2GRAY)
            
            # Create mask of pixels brighter than threshold
            bright_mask = (gray_roi > brightness_threshold).astype(np.uint8) * 255
            contours, _ = cv2.findContours(bright_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
            
            roi_area = bright_mask.size
            min_bright_area = max(1, int(roi_area * 0.005))
            
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
                    # Bright frame detected! This validates the CURRENT segment
                    state_bright_frame_detected = True
                    state_bright_frame_num = frame_count
                    print(f"Bright frame detected at frame {frame_count}")
                    
                    # IMPORTANT: If there's a PREVIOUS validated segment waiting for end_frame,
                    # finalize it NOW (this bright frame validates the current slate, meaning
                    # the previous segment should end before this slate started)
                    if (state_previous_segment['validated'] and 
                        state_previous_segment['start_frame'] is not None and
                        state_previous_segment['end_frame'] is None and
                        state_start_frame is not None):
                        # Set end_frame to just before current slate started
                        state_previous_segment['end_frame'] = state_start_frame - 1
                        print(f"  → Current slate validated - setting end_frame for PREVIOUS segment to {state_previous_segment['end_frame']}")
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
        
        # No bright frame detected, but return the ROI polygon for visualization
        return False, flash_roi_polygon
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
    global state_last_qr_data, state_segment_qr_data
    
    # Set segment QR if not yet set (first QR in this segment)
    if state_segment_qr_data is None:
        state_segment_qr_data = qr_data
        print(f"Segment QR code locked: {qr_data}")
    
    if state_last_qr_data != qr_data:
        state_last_qr_data = qr_data
        print(f"QR code detected: {qr_data}")

def visualize_frame(frame, slate_polygon, qr_polygon, flash_roi_polygon, bright_detected, frame_count):
    """Visualize detection results on frame"""
    frame_with_rect = frame.copy()
    
    # Display frame number in top-left corner
    cv2.putText(frame_with_rect, f"Frame: {frame_count}",
        (10, 30),
        cv2.FONT_HERSHEY_SIMPLEX, 1.0, (255, 255, 255), 2, cv2.LINE_AA)
    
    if slate_polygon is not None:
        polygon_int = slate_polygon.reshape(-1, 1, 2).astype(np.int32)
        if state_slate_detected:
            color = (0, 255, 0)
        else:
            color = (0, 0, 255)
        cv2.polylines(frame_with_rect, [polygon_int], isClosed=True, color=color, thickness=2)
        cv2.putText(frame_with_rect, "ARC-SLATE",
            tuple(slate_polygon[0].astype(int) + np.array([10, -20])),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
    
    if flash_roi_polygon is not None:
        flash_roi_int = flash_roi_polygon.reshape(-1, 1, 2).astype(np.int32)
        color = (0, 255, 0) if bright_detected else (0, 0, 255)
        cv2.polylines(frame_with_rect, [flash_roi_int], isClosed=True, color=color, thickness=2)
        cv2.putText(frame_with_rect, "flash_roi",
            tuple(flash_roi_polygon[0].astype(int) + np.array([5, -5])),
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2)
    
    if qr_polygon is not None:
        polygon_int = qr_polygon.reshape(-1, 1, 2).astype(np.int32)
        cv2.polylines(frame_with_rect, [polygon_int], isClosed=True, color=(0, 255, 0), thickness=2)
        
        # Display QR code label
        qr_label_pos = tuple(qr_polygon[0].astype(int))
        cv2.putText(frame_with_rect, "QR-Code",
            qr_label_pos,
            cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
        
        # Display QR code content if available
        if state_qr_data:
            qr_content_pos = (qr_label_pos[0], qr_label_pos[1] + 30)
            cv2.putText(frame_with_rect, state_qr_data,
                qr_content_pos,
                cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 255, 0), 2)
    
    cv2.imshow(f'Frame with ROI', frame_with_rect)
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

def create_xmp_sidecar(output_video_path, json_meta, current_code, current_roll, current_scene, current_shot, current_take, start_timecode, end_timecode, framerate):
    """Create XMP sidecar file with metadata from JSON"""
    xmp_path = output_video_path.replace('.mov', '.xmp')
    
    # Extract metadata from JSON
    production = json_meta.get("production", "")
    director = json_meta.get("director", "")
    camera_op = json_meta.get("camera", "")
    dop = json_meta.get("dop", "")
    
    description = ""
    environment = ""
    daynight = ""
    mos = False
    date = ""
    notes = ""
    good_take = False
    meters = ""
    
    for shot in json_meta.get("shotlist", []):
        if str(shot.get("shot")) == current_shot and shot.get("scene") == int(current_scene):
            description = shot.get("description", "")
            environment = shot.get("environment", "")
            daynight = shot.get("daynight", "")
            mos = shot.get("mos", False)
            
            for take in shot.get("takes", []):
                if str(take.get("take")) == current_take and take.get("roll") == current_roll:
                    date = take.get("date", "")
                    notes = take.get("notes", "")
                    good_take = take.get("goodTake", False)
                    meters = take.get("meters", "")
                    break
            break
    
    # Convert date to ISO 8601 format (YYYY-MM-DD) for XMP standard
    iso_date = date
    try:
        from datetime import datetime as dt
        parsed_date = dt.strptime(date, "%d.%m.%Y")
        iso_date = parsed_date.strftime("%Y-%m-%d")
    except:
        pass  # Keep original if conversion fails
    
    # Calculate duration in frames
    h, m, s, f = map(int, start_timecode.split(":"))
    start_frames = ((h * 3600 + m * 60 + s) * framerate) + f
    
    h, m, s, f = map(int, end_timecode.split(":"))
    end_frames = ((h * 3600 + m * 60 + s) * framerate) + f
    
    duration_frames = end_frames - start_frames
    
    # Generate unique IDs for Premiere Pro compatibility
    import uuid
    doc_id = str(uuid.uuid4())
    instance_id = str(uuid.uuid4())
    
    # Build XMP XML (Adobe Premiere Pro compatible format)
    # Note: xpacket wrapper and proper xmpMM fields are required for Premiere recognition
    xmp_content = f'''<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 7.0-c000">
  <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
    <rdf:Description rdf:about=""
        xmlns:dc="http://purl.org/dc/elements/1.1/"
        xmlns:xmp="http://ns.adobe.com/xap/1.0/"
        xmlns:xmpDM="http://ns.adobe.com/xmp/1.0/DynamicMedia/"
        xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"
        xmlns:stDim="http://ns.adobe.com/xap/1.0/sType/Dimensions#"
        xmlns:arc="http://arc-slate.com/ns/1.0/">
      
      <!-- XMP Media Management (required for Premiere Pro) -->
      <xmpMM:DocumentID>xmp.did:{doc_id}</xmpMM:DocumentID>
      <xmpMM:InstanceID>xmp.iid:{instance_id}</xmpMM:InstanceID>
      <xmpMM:OriginalDocumentID>xmp.did:{doc_id}</xmpMM:OriginalDocumentID>
      
      <!-- Dublin Core Metadata -->
      <dc:title>
        <rdf:Alt>
          <rdf:li xml:lang="x-default">{current_code}_{current_roll}_{current_scene}_{current_shot}_TAKE_{current_take}</rdf:li>
        </rdf:Alt>
      </dc:title>
      <dc:description>
        <rdf:Alt>
          <rdf:li xml:lang="x-default">{description}</rdf:li>
        </rdf:Alt>
      </dc:description>
      <dc:creator>
        <rdf:Seq>
          <rdf:li>{director}</rdf:li>
        </rdf:Seq>
      </dc:creator>
      <dc:subject>
        <rdf:Bag>
          <rdf:li>Scene {current_scene}</rdf:li>
          <rdf:li>Shot {current_shot}</rdf:li>
          <rdf:li>Take {current_take}</rdf:li>
        </rdf:Bag>
      </dc:subject>
      
      <!-- XMP Basic Metadata -->
      <xmp:CreateDate>{iso_date}</xmp:CreateDate>
      <xmp:CreatorTool>ARC-Slate Detection System</xmp:CreatorTool>
      <xmp:MetadataDate>{datetime.now().strftime('%Y-%m-%dT%H:%M:%S')}</xmp:MetadataDate>
      
      <!-- Dynamic Media (Video) Metadata - Premiere Pro format -->
      <xmpDM:videoFrameRate>{framerate}.000000</xmpDM:videoFrameRate>
      <xmpDM:videoFrameSize rdf:parseType="Resource">
        <stDim:w>1920</stDim:w>
        <stDim:h>1080</stDim:h>
        <stDim:unit>pixel</stDim:unit>
      </xmpDM:videoFrameSize>
      <xmpDM:videoFieldOrder>Progressive</xmpDM:videoFieldOrder>
      <xmpDM:videoPixelAspectRatio>1/1</xmpDM:videoPixelAspectRatio>
      <xmpDM:startTimecode rdf:parseType="Resource">
        <xmpDM:timeValue>{start_timecode}</xmpDM:timeValue>
        <xmpDM:timeFormat>24Timecode</xmpDM:timeFormat>
      </xmpDM:startTimecode>
      <xmpDM:altTimecode rdf:parseType="Resource">
        <xmpDM:timeValue>{start_timecode}</xmpDM:timeValue>
        <xmpDM:timeFormat>24Timecode</xmpDM:timeFormat>
      </xmpDM:altTimecode>
      <xmpDM:duration rdf:parseType="Resource">
        <xmpDM:value>{duration_frames}</xmpDM:value>
        <xmpDM:scale>1/{framerate}</xmpDM:scale>
      </xmpDM:duration>
      <xmpDM:startTimeScale>{framerate}</xmpDM:startTimeScale>
      <xmpDM:startTimeSampleSize>1</xmpDM:startTimeSampleSize>
      
      <!-- ARC-Slate Custom Metadata -->
      <arc:Production>{production}</arc:Production>
      <arc:ProjectCode>{current_code}</arc:ProjectCode>
      <arc:Director>{director}</arc:Director>
      <arc:DOP>{dop}</arc:DOP>
      <arc:CameraOperator>{camera_op}</arc:CameraOperator>
      <arc:Scene>{current_scene}</arc:Scene>
      <arc:Shot>{current_shot}</arc:Shot>
      <arc:Take>{current_take}</arc:Take>
      <arc:Roll>{current_roll}</arc:Roll>
      <arc:Environment>{environment}</arc:Environment>
      <arc:DayNight>{daynight}</arc:DayNight>
      <arc:MOS>{str(mos).lower()}</arc:MOS>
      <arc:DateRecorded>{iso_date}</arc:DateRecorded>
      <arc:StartTimecode>{start_timecode}</arc:StartTimecode>
      <arc:EndTimecode>{end_timecode}</arc:EndTimecode>
      <arc:Framerate>{framerate}</arc:Framerate>
      <arc:Notes>{notes}</arc:Notes>
      <arc:GoodTake>{str(good_take).lower()}</arc:GoodTake>
      <arc:FilmMeters>{meters}</arc:FilmMeters>
      
    </rdf:Description>
  </rdf:RDF>
</x:xmpmeta>
<?xpacket end="w"?>'''
    
    try:
        with open(xmp_path, 'w', encoding='utf-8') as f:
            f.write(xmp_content)
        print(f"XMP sidecar created: {xmp_path}")
    except Exception as e:
        print(f"Error creating XMP sidecar: {e}")

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
    """Get next available take from JSON chronologically (filtered by current roll, sorted by time/timecode)"""
    global state_current_roll
    
    if used_takes is None:
        used_takes = []
    
    # Collect all takes for the current roll with their metadata (excluding already used takes)
    all_takes = []
    for shot in json_meta.get("shotlist", []):
        for take in shot.get("takes", []):
            # Only consider takes that match the current roll
            if take.get("roll") == state_current_roll:
                take_data = {
                    'scene': str(shot.get("scene")),
                    'shot': str(shot.get("shot")),
                    'roll': take.get("roll"),
                    'take': str(take.get("take")),
                    'timecode': take.get("timecode", "00:00:00:00"),
                    'framerate': shot.get("framerate", 24),
                    'time': take.get("time", None)  # Optional time field
                }
                
                # Check if this take was already used
                take_key = f"{take_data['scene']}_{take_data['shot']}_{take_data['take']}_{take_data['roll']}"
                if take_key not in used_takes:
                    all_takes.append(take_data)
    
    # Sort takes chronologically
    def get_sort_key(take_entry):
        # Prefer 'time' field if available, otherwise use 'timecode'
        if take_entry['time']:
            return take_entry['time']
        else:
            # Convert timecode HH:MM:SS:FF to sortable string
            return take_entry['timecode']
    
    sorted_takes = sorted(all_takes, key=get_sort_key)
    
    print(f"DEBUG: get_next_take_from_json - Roll: {state_current_roll}, Available takes: {len(sorted_takes)}")
    print(f"DEBUG: Already used takes: {used_takes}")
    
    # Always take the first available take (index 0) since the list is already filtered
    if len(sorted_takes) > 0:
        take_info = sorted_takes[0]
        
        # Mark this take as used
        take_key = f"{take_info['scene']}_{take_info['shot']}_{take_info['take']}_{take_info['roll']}"
        if take_key not in used_takes:
            used_takes.append(take_key)
        
        print(f"DEBUG: Returning take {take_info['shot']}-{take_info['take']} (marked as used)")
        return take_info
    
    print(f"DEBUG: No more takes available for roll {state_current_roll}")
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
    """Synchronize take tracking with QR code (deprecated - counter system removed)"""
    # NOTE: Counter system removed - takes are now tracked via used_takes list only
    # This function is kept for compatibility but does nothing
    print(f"DEBUG: QR sync called (counter system deprecated, using used_takes list instead)")
    pass

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
            # Mark this take as used (important for QR code takes!)
            take_key = f"{take_info['scene']}_{take_info['shot']}_{take_info['take']}_{take_info['roll']}"
            if used_takes is not None and take_key not in used_takes:
                used_takes.append(take_key)
                print(f"DEBUG: QR take {take_info['shot']}-{take_info['take']} marked as used")
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
            current_code = json_meta.get("code", "PROJECT").split('_')[0]
    else:
        # Get code from JSON and extract only the number part (before _V)
        full_code = json_meta.get("code", "PROJECT")
        current_code = full_code.split('_')[0]
    
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
    print(f"Sync TC (from JSON, for bright frame at {sync_frame}): {sync_timecode}")
    
    # Berechne Start-Timecode: sync_timecode ist für den Bright Frame (sync_frame)
    # Video startet bei start_frame, also müssen wir die Differenz subtrahieren
    if sync_frame is not None and start_frame is not None:
        # Differenz zwischen bright frame und start frame
        delta_frames = -(sync_frame - start_frame)  # Negativ, weil wir zurückgehen
        start_timecode = adjust_timecode(sync_timecode, delta_frames, framerate)
        print(f"Start frame: {start_frame}, Sync frame (bright): {sync_frame}, Delta: {delta_frames} frames")
    else:
        start_timecode = sync_timecode
        print("Warning: sync_frame or start_frame is None, using sync_timecode as start_timecode")
    
    print(f"Start TC (for frame {start_frame}): {start_timecode}")

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

    # --- XMP Sidecar erzeugen ---
    end_timecode = adjust_timecode(start_timecode, end_frame - start_frame, framerate)
    #create_xmp_sidecar(output_mov, json_meta, current_code, current_roll, current_scene, current_shot, current_take, start_timecode, end_timecode, framerate)

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
    global state_start_frame, state_last_qr_data, state_segment_qr_data, state_used_takes, state_bright_frame_detected, state_current_roll
    
    # Initialize
    cap = cv2.VideoCapture(video_path)
    frame_count = 0
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    qreader = QReader('l')
    
    # Load JSON metadata
    load_json_metadata()
    
    # Detect which roll this video belongs to
    state_current_roll = detect_roll_from_video(video_path, state_json_meta)
    print(f"Processing video for roll: {state_current_roll}")
    
    # Initialize detector
    try:
        detector = SlateDetector(use_segmentation=True)
        print("Slate detector initialized successfully")
    except Exception as e:
        print(f"Error initializing slate detector: {e}")
        cap.release()
        return
    
    # Reset only frame-specific state (keep counter and used_takes persistent across videos)
    state_start_frame = None
    state_last_qr_data = None
    state_segment_qr_data = None
    state_slate_detection_buffer.clear()
    state_slate_polygon_history.clear()
    # DON'T reset: state_sequential_take_counter, state_used_takes, state_current_roll
    
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
                state_bright_frame_detected,
                frame_count
            )
    
    # Handle last segment: ALWAYS create a video if a bright frame was detected
    # Case 1: Previous segment waiting for end_frame (slate was lost, waiting for next validated slate)
    if (state_previous_segment['validated'] and 
        state_previous_segment['start_frame'] is not None and
        state_previous_segment['end_frame'] is None):
        state_previous_segment['end_frame'] = total_frames - 1
        print(f"Setting end_frame for PREVIOUS segment to {state_previous_segment['end_frame']} (end of video)")
        print(f"Creating LAST segment: frames {state_previous_segment['start_frame']}-{state_previous_segment['end_frame']}")
        finalize_slate_segment(
            video_path,
            state_previous_segment['start_frame'],
            state_previous_segment['end_frame'],
            state_previous_segment['bright_frame_num'],
            state_previous_segment['qr_code']
        )
    
    # Case 2: Current segment is still active and validated (slate still visible at end of video)
    # Use 'if' (not 'elif') to handle both cases independently
    if (state_slate_detected and state_bright_frame_detected and 
        state_start_frame is not None):
        print(f"Setting end_frame for CURRENT active segment to {total_frames - 1} (end of video)")
        print(f"Creating LAST segment: frames {state_start_frame}-{total_frames - 1}")
        finalize_slate_segment(
            video_path,
            state_start_frame,
            total_frames - 1,
            state_bright_frame_num,
            state_segment_qr_data
        )
    
    cap.release()
    print(f"Video processing completed: {frame_count} frames processed")

# --- Hauptablauf ---
import_folder = os.path.join(project_location, "01_IMPORT")
video_files = glob.glob(os.path.join(import_folder, "*.mov")) + \
              glob.glob(os.path.join(import_folder, "*.MOV"))

if not video_files:
    print("Error: No video files found in 01_IMPORT folder.")
    sys.exit(1)

# Sort video files by creation date (oldest first)
# Use st_birthtime for actual creation date on macOS (getctime returns metadata change time)
video_files_with_date = []
for vf in video_files:
    creation_time = os.stat(vf).st_birthtime  # Real creation date on macOS
    video_files_with_date.append((creation_time, vf))

video_files_sorted = sorted(video_files_with_date, key=lambda x: x[0])
print(f"\nFound {len(video_files_sorted)} video files in chronological order (by creation date).")

# Ask user for starting position
start_counter = 0
user_input = input(f"\nAb welchem Video möchten Sie beginnen? (1-{len(video_files_sorted)}, Enter für Start bei 1): ").strip()
if user_input:
    try:
        start_counter = int(user_input) - 1  # Convert to 0-based index
        if start_counter < 0 or start_counter >= len(video_files_sorted):
            print(f"Ungültige Eingabe. Starte bei Video 1.")
            start_counter = 0
        else:
            print(f"Starte bei Video {start_counter + 1} von {len(video_files_sorted)}")
    except ValueError:
        print(f"Ungültige Eingabe. Starte bei Video 1.")
        start_counter = 0
else:
    print(f"Starte bei Video 1 von {len(video_files_sorted)}")

# Pre-fill used_takes if starting from a later video
if start_counter > 0:
    print(f"\nLade JSON-Metadaten um die ersten {start_counter} Takes als verwendet zu markieren...")
    if load_json_metadata():
        # Detect roll from first video to be processed
        first_video_path = video_files_sorted[start_counter][1]
        state_current_roll = detect_roll_from_video(first_video_path, state_json_meta)
        
        if state_current_roll and state_json_meta:
            # Collect all takes for the current roll, sorted chronologically
            all_takes = []
            for shot in state_json_meta.get("shotlist", []):
                for take in shot.get("takes", []):
                    if take.get("roll") == state_current_roll:
                        take_data = {
                            'scene': str(shot.get("scene")),
                            'shot': str(shot.get("shot")),
                            'roll': take.get("roll"),
                            'take': str(take.get("take")),
                            'timecode': take.get("timecode", "00:00:00:00"),
                            'time': take.get("time", None)
                        }
                        all_takes.append(take_data)
            
            # Sort chronologically
            def get_sort_key(take_entry):
                if take_entry['time']:
                    return take_entry['time']
                else:
                    return take_entry['timecode']
            
            sorted_takes = sorted(all_takes, key=get_sort_key)
            
            # Mark first N takes as used
            takes_to_skip = min(start_counter, len(sorted_takes))
            for i in range(takes_to_skip):
                take = sorted_takes[i]
                take_key = f"{take['scene']}_{take['shot']}_{take['take']}_{take['roll']}"
                state_used_takes.append(take_key)
                print(f"  Markiere Take {take['shot']}-{take['take']} als bereits verwendet")
            
            print(f"  {takes_to_skip} Takes als verwendet markiert (für Roll {state_current_roll})")
        else:
            print(f"  Warnung: Roll konnte nicht erkannt werden oder JSON fehlt")

print(f"\nProcessing {len(video_files_sorted) - start_counter} video files...")

for idx, (creation_time, video_path) in enumerate(video_files_sorted):
    if idx < start_counter:
        continue  # Skip videos before start_counter
    
    print(f"\n--- Verarbeitung von Video {idx + 1}/{len(video_files_sorted)}: {video_path} (created: {datetime.fromtimestamp(creation_time)}) ---")
    process_video(video_path, True)

print(f"\n=== All videos processed. Total takes used: {len(state_used_takes)} ===")
