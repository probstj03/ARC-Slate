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
state_raw_yolo_detections = []  # Buffer for raw YOLO detections (to detect consistent new positions)
state_flash_detection_buffer = []  # Buffer for 2-frame flash detection (stores frame numbers)
state_previous_flash_roi = None  # Previous frame's flash ROI for temporal gradient detection
state_previous_flash_roi_gray = None  # Previous frame's flash ROI in grayscale
state_flash_start_frame = None  # Frame number where flash started (for duration check)
state_flash_duration = 0  # How many consecutive frames the flash has been detected
state_flash_baseline_frames = []  # First 10 frames to calculate baseline brightness

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
    """Reset all global state variables for next video"""
    global state_slate_detected, state_qr_detected, state_bright_frame_detected
    global state_slate_polygon, state_qr_data, state_bright_frame_num, state_start_frame
    global state_last_qr_data, state_segment_qr_data
    global state_slate_detection_buffer, state_slate_polygon_history, state_raw_yolo_detections
    global state_flash_detection_buffer, state_previous_flash_roi, state_previous_flash_roi_gray
    global state_previous_segment, state_flash_baseline_frames
    global state_flash_start_frame, state_flash_duration
    
    state_slate_detected = False
    state_qr_detected = False
    state_bright_frame_detected = False
    state_slate_polygon = None
    state_qr_data = None
    state_bright_frame_num = None
    state_start_frame = None
    state_last_qr_data = None
    state_segment_qr_data = None
    state_slate_detection_buffer = []
    state_slate_polygon_history = []
    state_raw_yolo_detections = []
    state_flash_detection_buffer = []
    state_previous_flash_roi = None
    state_previous_flash_roi_gray = None
    state_flash_baseline_frames = []
    state_flash_start_frame = None
    state_flash_duration = 0
    
    state_previous_segment = {
        'start_frame': None,
        'end_frame': None,
        'bright_frame_num': None,
        'qr_code': None,
        'validated': False
    }
    
    # DON'T reset: state_used_takes, state_current_roll, state_json_meta

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
    """Calculate the maximum perspective tilt angle of the polygon"""
    if polygon is None or len(polygon) < 4:
        return 999  # Invalid polygon
    
    # Calculate angles between consecutive edges (should be ~90° for a rectangle)
    edges = []
    for i in range(4):
        p1 = polygon[i]
        p2 = polygon[(i + 1) % 4]
        edge = p2 - p1
        edges.append(edge)
    
    # Check all corner angles
    max_deviation = 0
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
            
            # Deviation from 90 degrees (perfect rectangle)
            deviation = abs(90 - angle)
            max_deviation = max(max_deviation, deviation)
    
    return max_deviation

def validate_rectangle_geometry(polygon):
    """Validate that polygon represents a physically plausible rectangle"""
    if polygon is None or len(polygon) < 4:
        return False
    
    # Calculate edge lengths
    edges = []
    for i in range(4):
        p1 = polygon[i]
        p2 = polygon[(i + 1) % 4]
        length = np.linalg.norm(p2 - p1)
        edges.append(length)
    
    # Opposite sides should be similar in length (within 30% tolerance)
    # For a rectangle: edge[0] ≈ edge[2] and edge[1] ≈ edge[3]
    ratio_1 = min(edges[0], edges[2]) / max(edges[0], edges[2]) if max(edges[0], edges[2]) > 0 else 0
    ratio_2 = min(edges[1], edges[3]) / max(edges[1], edges[3]) if max(edges[1], edges[3]) > 0 else 0
    
    if ratio_1 < 0.7 or ratio_2 < 0.7:
        return False  # Opposite sides too different
    
    # Check aspect ratio (slate is wider than tall, typically 1.5:1 to 3:1)
    width = max(edges[0], edges[2])
    height = max(edges[1], edges[3])
    aspect_ratio = width / height if height > 0 else 0
    
    if aspect_ratio < 1.2 or aspect_ratio > 4.0:
        return False  # Unrealistic aspect ratio for a slate
    
    return True

def polygon_changed_significantly(poly1, poly2, threshold=50):
    """Check if polygon changed significantly between frames"""
    if poly1 is None or poly2 is None:
        return True
    
    # Calculate average distance between corresponding points
    distances = [np.linalg.norm(poly1[i] - poly2[i]) for i in range(4)]
    avg_distance = np.mean(distances)
    
    return avg_distance > threshold

def detect_single_point_outlier(poly1, poly2, threshold=50):
    """Detect if only a single point moved significantly (outlier detection)"""
    if poly1 is None or poly2 is None:
        return False
    
    # Calculate distance for each point
    distances = [np.linalg.norm(poly1[i] - poly2[i]) for i in range(4)]
    
    # Count how many points moved significantly
    moved_points = sum(1 for d in distances if d > threshold)
    
    # If only 1 or 2 points moved, it's likely an outlier (slate doesn't tilt this fast)
    # Physical constraint: slate can't rotate around a single point that quickly
    if moved_points <= 2:
        return True
    
    return False

def stabilize_polygon(new_polygon, max_angle=30, max_change=10):
    """Stabilize polygon detection with extremely strict geometric validation - prevents single point jumps"""
    global state_slate_polygon_history, state_raw_yolo_detections
    
    if new_polygon is None:
        return None
    
    # Track raw YOLO detections to detect consistent new positions
    state_raw_yolo_detections.append(new_polygon.copy())
    if len(state_raw_yolo_detections) > 10:
        state_raw_yolo_detections.pop(0)
    
    # CRITICAL: If YOLO has detected a consistent position for 5+ frames (even if rejected), accept it
    # This handles the case where the slate moves to a new position and stays there
    if len(state_raw_yolo_detections) >= 5:
        # Check if last 5 YOLO detections are all very similar to new_polygon
        all_consistent = True
        for raw_det in state_raw_yolo_detections[-5:]:
            det_distances = [np.linalg.norm(raw_det[i] - new_polygon[i]) for i in range(4)]
            if np.max(det_distances) > 8:  # Allow 8px tolerance
                all_consistent = False
                break
        
        if all_consistent:
            # YOLO has been consistent for 5 frames - force accept and reset history
            state_slate_polygon_history = [new_polygon]
            # print(f"  → YOLO consistent for 5+ frames - forcing position update")
            return new_polygon
    
    # Check if new polygon has been consistent for multiple frames (even if geometry/angle changed)
    # If the same polygon appears 5+ times in a row, accept it regardless of validation
    if len(state_slate_polygon_history) >= 5:
        # Check if last 5 detections are all very similar to new_polygon
        all_similar = True
        for hist_poly in state_slate_polygon_history[-5:]:
            poly_distances = [np.linalg.norm(hist_poly[i] - new_polygon[i]) for i in range(4)]
            if np.max(poly_distances) > 5:  # Allow 5px tolerance
                all_similar = False
                break
        
        if all_similar:
            # Same position for 5+ frames - accept it, override geometry/angle validation
            state_slate_polygon_history.append(new_polygon)
            # print(f"  → Position consistent over 5+ frames - accepting despite geometry changes")
            return new_polygon
    
    # Step 1: Validate rectangle geometry (only for new positions)
    if not validate_rectangle_geometry(new_polygon):
        # Invalid geometry, use previous polygon if available
        if len(state_slate_polygon_history) > 0:
            return state_slate_polygon_history[-1]
        else:
            return None
    
    # Step 2: Check angle constraint (max 30° tilt, only for new positions)
    angle_deviation = calculate_polygon_angle(new_polygon)
    if angle_deviation > max_angle:
        # Angle too steep, use previous polygon if available
        if len(state_slate_polygon_history) > 0:
            return state_slate_polygon_history[-1]
        else:
            return None
    
    # Step 3: Analyze point movement
    if len(state_slate_polygon_history) > 0:
        last_polygon = state_slate_polygon_history[-1]
        
        # Calculate which points moved significantly
        distances = [np.linalg.norm(last_polygon[i] - new_polygon[i]) for i in range(4)]
        moved_points = sum(1 for d in distances if d > max_change)
        
        # Check if angle changed significantly (rotation vs pure translation)
        old_angle = calculate_polygon_angle(last_polygon)
        new_angle = calculate_polygon_angle(new_polygon)
        angle_change = abs(new_angle - old_angle)
        has_rotation = angle_change > 5  # More than 5° change = rotation
     
        # CRITICAL: Single point movement is almost NEVER valid - reject unless 6+ frames consistent
        if moved_points == 1:
            if len(state_slate_polygon_history) >= 6:
                # Check if SAME single point has been moving consistently for 6 frames
                moving_point_idx = next(i for i, d in enumerate(distances) if d > max_change)
                consistent = True
                
                for hist_idx in range(len(state_slate_polygon_history) - 5, len(state_slate_polygon_history)):
                    if hist_idx >= 0 and hist_idx + 1 < len(state_slate_polygon_history):
                        hist_poly = state_slate_polygon_history[hist_idx]
                        next_hist_poly = state_slate_polygon_history[hist_idx + 1]
                        hist_dist = np.linalg.norm(hist_poly[moving_point_idx] - next_hist_poly[moving_point_idx])
                        
                        # Same point must be moving in each frame
                        if hist_dist <= max_change:
                            consistent = False
                            break
                
                if consistent:
                    state_slate_polygon_history.append(new_polygon)
                    # print(f"  → Single point movement accepted after 6 frames (point {moving_point_idx})")
                else:
                    return last_polygon
            else:
                # Not enough frames, reject
                # print(f"  → Single point movement rejected (need 6 consistent frames)")
                return last_polygon
        
        # Two points moved - accept immediately (likely valid movement)
        elif moved_points == 2:
            state_slate_polygon_history.append(new_polygon)
            # print(f"  → 2-point movement accepted immediately (rotation: {has_rotation})")
            return new_polygon
        
        # 3 points moved - accept immediately (likely valid movement)
        elif moved_points == 3:
            state_slate_polygon_history.append(new_polygon)
            # print(f"  → 3-point movement accepted immediately (rotation: {has_rotation})")
            return new_polygon
        
        # All 4 points moved - check if it's uniform translation (entire slate moving)
        elif moved_points == 4:
            # Check if all points moved in roughly the same direction (uniform translation)
            # Calculate movement vectors for each point
            movement_vectors = [new_polygon[i] - last_polygon[i] for i in range(4)]
            avg_movement = np.mean(movement_vectors, axis=0)
            
            # Check if all movements are roughly similar (uniform translation)
            movement_deviations = [np.linalg.norm(mv - avg_movement) for mv in movement_vectors]
            max_deviation = np.max(movement_deviations)
            
            # If deviation is reasonable (< 15px), it's roughly uniform translation - accept immediately
            if max_deviation < 15:
                # Roughly uniform translation detected - accept any movement (no minimum threshold)
                state_slate_polygon_history.append(new_polygon)
                # print(f"  → Uniform translation accepted (movement: {np.linalg.norm(avg_movement):.1f}px, deviation: {max_deviation:.1f}px)")
                return new_polygon
            else:
                # Not uniform translation but all 4 points moving - accept immediately (rotation/perspective change)
                state_slate_polygon_history.append(new_polygon)
                # print(f"  → Full polygon movement accepted immediately (rotation: {has_rotation}, deviation: {max_deviation:.1f}px)")
                return new_polygon
        else:
            # No significant movement, accept immediately
            state_slate_polygon_history.append(new_polygon)
    else:
        # First polygon, add to history
        state_slate_polygon_history.append(new_polygon)
    
    # Keep only last 7 polygons in history (reduced from 11)
    if len(state_slate_polygon_history) > 7:
        state_slate_polygon_history.pop(0)
    
    return new_polygon

def detect_slate(frame, detector, frame_count, video_path):
    """Detect slate/clapperboard in frame with 3-frame stability buffer"""
    global state_slate_detected, state_slate_polygon, state_start_frame, state_slate_detection_buffer
    global state_bright_frame_detected, state_bright_frame_num, state_last_qr_data, state_segment_qr_data, state_previous_segment
    global state_slate_polygon_history, state_raw_yolo_detections
    global state_flash_calibration_counter, state_flash_max_brightness_seen
    
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
            
            # CRITICAL: Reset polygon history and raw detections for NEW slate
            state_slate_polygon_history.clear()
            state_raw_yolo_detections.clear()
            
            # Reset flash calibration for new slate (new scene/lighting conditions)
            state_flash_calibration_counter = 0
            state_flash_max_brightness_seen = 0
            state_flash_start_frame = None
            state_flash_duration = 0
            
            # Set new slate as detected
            state_slate_detected = True
            state_start_frame = new_start_frame
            state_bright_frame_detected = False
            state_bright_frame_num = None
            state_last_qr_data = None
            state_segment_qr_data = None  # Reset segment QR at start of new segment
            print(f"Frame {frame_count}: Slate detected - start_frame={state_start_frame} (polygon history reset, flash calibration reset)")
            
        # All 3 frames without slate -> slate disappears (just log, don't set end_frame)
        elif not any(state_slate_detection_buffer) and state_slate_detected:
            print(f"Frame {frame_count}: Slate lost")
            
            # CRITICAL: Only save to state_previous_segment if THIS segment was validated!
            # Non-validated segments should NOT overwrite a previous validated segment
            if state_bright_frame_detected:
                # This segment was validated (has bright frame) - save it
                # SAFETY CHECK: Only save if start_frame is valid
                if state_start_frame is not None:
                    state_previous_segment = {
                        'start_frame': state_start_frame,
                        'end_frame': None,  # Will be set when next validated slate appears
                        'bright_frame_num': state_bright_frame_num,
                        'qr_code': state_segment_qr_data,  # Use segment-wide QR code
                        'validated': True
                    }
                    print(f"  → Segment validated (bright frame at {state_bright_frame_num}) - waiting for next validated slate to set end_frame")
                else:
                    print(f"  → WARNING: Bright frame detected but start_frame is None - cannot save segment!")
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
        # CRITICAL: Apply stabilization to prevent polygon jumping
        stabilized_polygon = stabilize_polygon(polygon)
        if stabilized_polygon is not None:
            state_slate_polygon = stabilized_polygon
        return True, state_slate_polygon
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
    """
    Robust flash detection using:
    1. Temporal Gradient (relative brightness change between frames)
    2. Multi-Resolution Analysis (HSV Saturation filter to exclude red displays)
    3. Circularity check (flash must be circular)
    4. Duration check (flash must last 1-3 frames)
    """
    global state_bright_frame_detected, state_bright_frame_num, state_previous_segment
    global state_previous_flash_roi, state_previous_flash_roi_gray
    global state_flash_start_frame, state_flash_duration, state_flash_baseline_frames
    
    if slate_polygon is None:
        state_previous_flash_roi = None
        state_previous_flash_roi_gray = None
        return False, None
    
    try:
        # Define flash ROI (top-left corner of slate)
        flash_roi_norm = np.array([
            [0.0, 0.0],
            [0.2, 0.0],
            [0.2, 0.55],
            [0.0, 0.55]
        ], dtype=np.float32)
        
        normalized_quad = np.array([
            [0, 0],
            [1, 0],
            [1, 1],
            [0, 1]
        ], dtype=np.float32)
        
        # Transform ROI to match slate perspective
        H, _ = cv2.findHomography(normalized_quad, slate_polygon)
        flash_roi_norm_reshaped = flash_roi_norm.reshape(-1, 1, 2).astype(np.float32)
        flash_roi_transformed = cv2.perspectiveTransform(flash_roi_norm_reshaped, H)
        flash_roi_polygon = flash_roi_transformed.reshape(-1, 2)
        
        # Extract bounding box
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
        
        if flash_roi.size == 0:
            return False, flash_roi_polygon
        
        # Convert to grayscale and HSV for multi-resolution analysis
        flash_roi_gray = cv2.cvtColor(flash_roi, cv2.COLOR_BGR2GRAY)
        flash_roi_hsv = cv2.cvtColor(flash_roi, cv2.COLOR_BGR2HSV)
        
        # PHASE 1: Baseline calibration (first 2 frames - very quick calibration)
        if len(state_flash_baseline_frames) < 2:
            state_flash_baseline_frames.append(flash_roi_gray.copy())
            state_previous_flash_roi_gray = flash_roi_gray.copy()
            print(f"  → Flash calibration: {len(state_flash_baseline_frames)}/2 frames collected")
            return False, flash_roi_polygon
        
        # Calculate baseline from first 2 frames
        if len(state_flash_baseline_frames) == 2 and state_previous_flash_roi_gray is not None:
            baseline_brightness = np.median([np.median(f) for f in state_flash_baseline_frames])
            print(f"  → Flash detection calibrated: Baseline brightness = {baseline_brightness:.1f}")
            state_flash_baseline_frames.append(None)  # Mark as calibrated
        
        # PHASE 2: Flash detection
        flash_detected_this_frame = False
        
        if state_previous_flash_roi_gray is not None and state_previous_flash_roi_gray.shape == flash_roi_gray.shape:
            # ====================
            # DETECTOR 1: Temporal Gradient (brightness change)
            # ====================
            frame_diff = cv2.absdiff(flash_roi_gray, state_previous_flash_roi_gray)
            mean_diff = np.mean(frame_diff)
            max_diff = np.max(frame_diff)
            
            # Flash = significant brightness increase OR decrease (flash ending)
            brightness_increase = np.mean(flash_roi_gray) - np.mean(state_previous_flash_roi_gray)
            brightness_change_abs = abs(brightness_increase)
            
            # Detect both flash start (brightness increase) AND flash end (brightness decrease)
            # Stricter thresholds to avoid false positives
            temporal_gradient_detected = ((brightness_change_abs > 15 and max_diff > 80) or 
                                         (brightness_change_abs > 10 and max_diff > 100) or
                                         (brightness_change_abs > 5 and max_diff > 120))
            
            # Debug: Show temporal gradient info when significant change detected
            if brightness_change_abs > 3 or max_diff > 30:
                direction = "↑" if brightness_increase > 0 else "↓"
                print(f"  [Frame {frame_count}] Brightness {direction}{brightness_change_abs:.1f}, max_diff: {max_diff:.1f} {'✓' if temporal_gradient_detected else '✗'}")
            
            # ====================
            # DETECTOR 2: Saturation Filter (exclude red displays)
            # ====================
            s_channel = flash_roi_hsv[:, :, 1]  # Saturation channel
            low_saturation_mask = (s_channel < 80).astype(np.uint8) * 255  # Relaxed: 80 instead of 50
            
            # ====================
            # DETECTOR 3: Brightness threshold (absolute) - very lenient for dark flashes
            # ====================
            _, bright_mask = cv2.threshold(flash_roi_gray, 100, 255, cv2.THRESH_BINARY)  # Lowered from 120 to 100
            
            # ====================
            # COMBINE: Must be bright AND low saturation (gray) AND temporal change
            # ====================
            flash_candidate_mask = cv2.bitwise_and(bright_mask, low_saturation_mask)
            
            # Debug: Count candidate pixels
            candidate_count = cv2.countNonZero(flash_candidate_mask)
            if candidate_count > 0 and temporal_gradient_detected:
                print(f"  [Frame {frame_count}] Flash candidates: {candidate_count} pixels (bright + gray + temporal)")
            
            # Only proceed if temporal gradient was detected
            if temporal_gradient_detected:
                # Find contours of candidate regions
                contours, _ = cv2.findContours(flash_candidate_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
                
                # Calculate ROI dimensions for circularity check
                roi_width = flash_roi_gray.shape[1]
                roi_area = flash_roi_gray.size
                min_diameter = roi_width * 0.08  # Very small minimum
                min_radius = min_diameter / 2.0
                min_area = np.pi * (min_radius ** 2)
                max_area = roi_area * 0.85  # Allow large regions
                
                # ====================
                # DETECTOR 4: Circularity Check
                # ====================
                valid_regions = []
                for i, cnt in enumerate(contours):
                    area = cv2.contourArea(cnt)
                    
                    # Debug: Show all contours found
                    if area > 0:
                        (cx, cy), radius = cv2.minEnclosingCircle(cnt)
                        circle_area = np.pi * (radius ** 2)
                        circularity = area / circle_area if circle_area > 0 else 0
                        diameter = radius * 2
                        
                        print(f"    Contour {i+1}: area={area:.0f} (min={min_area:.0f}, max={max_area:.0f}), "
                              f"circularity={circularity:.2f}, diameter={diameter:.1f} (min={min_diameter:.1f})")
                    
                    if min_area <= area <= max_area:
                        (cx, cy), radius = cv2.minEnclosingCircle(cnt)
                        circle_area = np.pi * (radius ** 2)
                        circularity = area / circle_area if circle_area > 0 else 0
                        
                        # Very relaxed circularity: 0.25 for difficult cases
                        if circularity > 0.25 and radius * 2 >= min_diameter:
                            valid_regions.append(cnt)
                            print(f"    ✓ Valid circular region: area={area:.0f}, circularity={circularity:.2f}")
                
                # Flash detected if ONE TO FIVE valid circular regions (allow fragmented flash)
                flash_detected_this_frame = (1 <= len(valid_regions) <= 5)
                
                if len(valid_regions) > 5:
                    print(f"  [Frame {frame_count}] Too many regions ({len(valid_regions)}) - ambiguous, skipping")
                elif len(valid_regions) == 0 and len(contours) > 0:
                    print(f"  [Frame {frame_count}] No valid circular regions found ({len(contours)} candidates)")
        
        # ====================
        # DETECTOR 5: Duration Check (1-3 frames)
        # ====================
        if flash_detected_this_frame:
            if state_flash_start_frame is None:
                state_flash_start_frame = frame_count
                state_flash_duration = 1
                print(f"  ⚡ Flash started at frame {frame_count}")
            else:
                state_flash_duration += 1
                
                # Flash too long = invalid
                if state_flash_duration > 3:
                    print(f"  ✗ Flash exceeded 3 frames - INVALID")
                    state_flash_start_frame = None
                    state_flash_duration = 0
                    state_previous_flash_roi_gray = flash_roi_gray.copy()
                    return False, flash_roi_polygon
        else:
            # Flash ended
            if state_flash_start_frame is not None:
                if 1 <= state_flash_duration <= 3:
                    # Valid flash detected!
                    print(f"  ✓ Flash DETECTED: frame {state_flash_start_frame} (duration: {state_flash_duration} frames)")
                    state_bright_frame_detected = True
                    state_bright_frame_num = frame_count
                    
                    # Finalize previous segment if needed
                    if (state_previous_segment['validated'] and 
                        state_previous_segment['start_frame'] is not None and
                        state_previous_segment['end_frame'] is None and
                        state_start_frame is not None):
                        state_previous_segment['end_frame'] = state_start_frame - 1
                        print(f"  → Finalizing previous segment: frames {state_previous_segment['start_frame']}-{state_previous_segment['end_frame']}")
                        finalize_slate_segment(
                            video_path,
                            state_previous_segment['start_frame'],
                            state_previous_segment['end_frame'],
                            state_previous_segment['bright_frame_num'],
                            state_previous_segment['qr_code']
                        )
                        
                        state_previous_segment = {
                            'start_frame': None,
                            'end_frame': None,
                            'bright_frame_num': None,
                            'qr_code': None,
                            'validated': False
                        }
                    
                    state_flash_start_frame = None
                    state_flash_duration = 0
                    state_previous_flash_roi_gray = flash_roi_gray.copy()
                    return True, flash_roi_polygon
                else:
                    print(f"  ✗ Flash duration invalid ({state_flash_duration} frames)")
                
                state_flash_start_frame = None
                state_flash_duration = 0
        
        # Update previous frame for next comparison
        state_previous_flash_roi_gray = flash_roi_gray.copy()
        return False, flash_roi_polygon
    
    except Exception as e:
        print(f"  ✗ Flash detection error: {e}")
        return False, None
        return False, flash_roi_polygon
    except Exception as e:
        print(f"Error detecting bright frame: {e}")
        state_previous_flash_roi = None
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
    """Create XMP sidecar file with metadata from JSON - Adobe Premiere Pro compatible"""
    import uuid
    xmp_path = output_video_path.replace('.mov', '.xmp')
    
    # Extract metadata from JSON
    production = json_meta.get("production", "")
    director = json_meta.get("director", "")
    dop = json_meta.get("camera", "")  # camera field = DOP (Director of Photography)
    
    description = ""
    environment = ""
    daynight = ""
    date = ""
    notes = ""
    good_take = False
    
    for shot in json_meta.get("shotlist", []):
        if str(shot.get("shot")) == current_shot and shot.get("scene") == int(current_scene):
            description = shot.get("description", "")
            environment = shot.get("environment", "")
            daynight = shot.get("daynight", "")
            
            for take in shot.get("takes", []):
                if str(take.get("take")) == current_take and take.get("roll") == current_roll:
                    date = take.get("date", "")
                    notes = take.get("notes", "")
                    good_take = take.get("goodTake", False)
                    break
            break
    
    # Convert date to ISO 8601 format with time
    iso_date = ""
    try:
        from datetime import datetime as dt
        if date and '-' not in date:
            # Convert from DD.MM.YYYY to YYYY-MM-DDTHH:MM:SS
            parsed_date = dt.strptime(date, '%d.%m.%Y')
            # Add timecode as time
            h, m, s, _ = map(int, start_timecode.split(":"))
            iso_date = f"{parsed_date.strftime('%Y-%m-%d')}T{h:02d}:{m:02d}:{s:02d}"
        elif date:
            iso_date = date
    except:
        iso_date = datetime.now().strftime('%Y-%m-%dT%H:%M:%S')
    
    # Calculate duration in 90000 timescale (standard for video)
    h, m, s, f = map(int, start_timecode.split(":"))
    start_frames = ((h * 3600 + m * 60 + s) * framerate) + f
    
    h, m, s, f = map(int, end_timecode.split(":"))
    end_frames = ((h * 3600 + m * 60 + s) * framerate) + f
    
    duration_frames = end_frames - start_frames
    duration_90k = int((duration_frames / framerate) * 90000)
    
    # Generate unique IDs
    doc_id = str(uuid.uuid4())
    instance_id = str(uuid.uuid4())
    original_doc_id = str(uuid.uuid4())
    
    # Current timestamp
    now = datetime.now()
    timestamp = now.strftime('%Y-%m-%dT%H:%M:%S+01:00')
    
    # Build XMP XML matching the working example
    xmp_content = f'''<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 9.1-c003 79.9690a87, 2025/03/06-19:12:03        ">
   <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
      <rdf:Description rdf:about=""
            xmlns:xmp="http://ns.adobe.com/xap/1.0/"
            xmlns:xmpDM="http://ns.adobe.com/xmp/1.0/DynamicMedia/"
            xmlns:stDim="http://ns.adobe.com/xap/1.0/sType/Dimensions#"
            xmlns:tiff="http://ns.adobe.com/tiff/1.0/"
            xmlns:xmpMM="http://ns.adobe.com/xap/1.0/mm/"
            xmlns:stEvt="http://ns.adobe.com/xap/1.0/sType/ResourceEvent#"
            xmlns:dc="http://purl.org/dc/elements/1.1/">
         <xmp:CreateDate>{timestamp}</xmp:CreateDate>
         <xmp:ModifyDate>{timestamp}</xmp:ModifyDate>
         <xmp:MetadataDate>{timestamp}</xmp:MetadataDate>
         <xmp:CreatorTool>ARC-Slate Detection System</xmp:CreatorTool>
         <xmpDM:duration rdf:parseType="Resource">
            <xmpDM:value>{duration_90k}</xmpDM:value>
            <xmpDM:scale>1/90000</xmpDM:scale>
         </xmpDM:duration>
         <xmpDM:startTimeScale>{int(framerate)}</xmpDM:startTimeScale>
         <xmpDM:startTimeSampleSize>1</xmpDM:startTimeSampleSize>
         <xmpDM:videoAlphaUnityIsTransparent>False</xmpDM:videoAlphaUnityIsTransparent>
         <xmpDM:scene>{current_scene}</xmpDM:scene>
         <xmpDM:shotName>{production}</xmpDM:shotName>
         <xmpDM:altTimecode rdf:parseType="Resource">
            <xmpDM:timeValue>{start_timecode}</xmpDM:timeValue>
            <xmpDM:timeFormat>24Timecode</xmpDM:timeFormat>
         </xmpDM:altTimecode>'''
    
    # Add optional fields only if they have values
    if notes:
        xmp_content += f'''
         <xmpDM:logComment>{notes}</xmpDM:logComment>'''
    
    xmp_content += f'''
         <xmpDM:good>{str(good_take).title()}</xmpDM:good>'''
    
    if production:
        xmp_content += f'''
         <xmpDM:projectName>{production}</xmpDM:projectName>'''
    
    if director:
        xmp_content += f'''
         <xmpDM:director>{director}</xmpDM:director>'''
    
    if dop:
        xmp_content += f'''
         <xmpDM:directorPhotography>{dop}</xmpDM:directorPhotography>'''
    
    if date:
        # Convert date format for shotDay field
        try:
            from datetime import datetime as dt
            if '-' not in date:
                parsed = dt.strptime(date, '%d.%m.%Y')
                shot_day = parsed.strftime('%d.%m.%Y')
            else:
                shot_day = date
        except:
            shot_day = date
        
        xmp_content += f'''
         <xmpDM:shotDay>{shot_day}</xmpDM:shotDay>'''
    
    if current_roll:
        xmp_content += f'''
         <xmpDM:tapeName>{current_roll}</xmpDM:tapeName>'''
    
    if iso_date:
        xmp_content += f'''
         <xmpDM:shotDate>{iso_date}</xmpDM:shotDate>'''
    
    xmp_content += f'''
         <xmpDM:shotNumber>{current_shot} {current_take}</xmpDM:shotNumber>
         <xmpDM:projectRef rdf:parseType="Resource">
            <xmpDM:type>movie</xmpDM:type>
         </xmpDM:projectRef>
         <xmpDM:videoFrameSize rdf:parseType="Resource">
            <stDim:w>1920</stDim:w>
            <stDim:h>1080</stDim:h>
            <stDim:unit>pixel</stDim:unit>
         </xmpDM:videoFrameSize>
         <xmpDM:videoFrameRate>{framerate:.6f}</xmpDM:videoFrameRate>
         <xmpDM:videoFieldOrder>Progressive</xmpDM:videoFieldOrder>
         <xmpDM:videoPixelAspectRatio>1/1</xmpDM:videoPixelAspectRatio>
         <xmpDM:startTimecode rdf:parseType="Resource">
            <xmpDM:timeFormat>24Timecode</xmpDM:timeFormat>
            <xmpDM:timeValue>{start_timecode}</xmpDM:timeValue>
         </xmpDM:startTimecode>
         <tiff:Orientation>1</tiff:Orientation>
         <xmpMM:InstanceID>xmp.iid:{instance_id}</xmpMM:InstanceID>
         <xmpMM:DocumentID>{doc_id}</xmpMM:DocumentID>
         <xmpMM:OriginalDocumentID>xmp.did:{original_doc_id}</xmpMM:OriginalDocumentID>
         <dc:format>H.264</dc:format>
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

def mark_previous_takes_as_used(json_meta, current_take_info, used_takes, current_roll):
    """Mark all takes that came before the current take (chronologically) as used"""
    if used_takes is None:
        return
    
    # Collect all takes for the current roll with their metadata
    all_takes = []
    for shot in json_meta.get("shotlist", []):
        for take in shot.get("takes", []):
            if take.get("roll") == current_roll:
                take_data = {
                    'scene': str(shot.get("scene")),
                    'shot': str(shot.get("shot")),
                    'roll': take.get("roll"),
                    'take': str(take.get("take")),
                    'timecode': take.get("timecode", "00:00:00:00"),
                    'time': take.get("time", None)
                }
                all_takes.append(take_data)
    
    # Sort takes chronologically
    def get_sort_key(take_entry):
        if take_entry['time']:
            return take_entry['time']
        else:
            return take_entry['timecode']
    
    sorted_takes = sorted(all_takes, key=get_sort_key)
    
    # Find the current take in the sorted list
    current_take_key = f"{current_take_info['scene']}_{current_take_info['shot']}_{current_take_info['take']}_{current_take_info['roll']}"
    
    # Mark all takes before the current one as used
    marked_count = 0
    for take in sorted_takes:
        take_key = f"{take['scene']}_{take['shot']}_{take['take']}_{take['roll']}"
        
        # Stop when we reach the current take
        if take_key == current_take_key:
            break
        
        # Mark earlier takes as used
        if take_key not in used_takes:
            used_takes.append(take_key)
            marked_count += 1
            print(f"  → Marked earlier take {take['shot']}-{take['take']} as used")
    
    if marked_count > 0:
        print(f"  → Total: {marked_count} earlier take(s) marked as used")

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
            # Mark all takes BEFORE this one as used (chronologically)
            if used_takes is not None:
                mark_previous_takes_as_used(json_meta, take_info, used_takes, take_info['roll'])
            
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
    # create_xmp_sidecar(output_mov, json_meta, current_code, current_roll, current_scene, current_shot, current_take, start_timecode, end_timecode, framerate)

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
    global state_flash_calibration_counter, state_flash_max_brightness_seen, state_flash_brightness_threshold
    global state_slate_detected, state_slate_polygon, state_qr_detected, state_qr_data, state_bright_frame_num, state_previous_segment
    
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
    
    # CRITICAL: Reset ALL state variables for each video (prevent contamination from previous video)
    state_slate_detected = False
    state_qr_detected = False
    state_bright_frame_detected = False
    state_slate_polygon = None
    state_qr_data = None
    state_bright_frame_num = None
    state_start_frame = None
    state_last_qr_data = None
    state_segment_qr_data = None
    state_slate_detection_buffer.clear()
    state_slate_polygon_history.clear()
    state_raw_yolo_detections.clear()
    state_previous_flash_roi = None
    state_flash_detection_buffer.clear()
    
    # Reset previous segment (important: don't carry over from previous video!)
    state_previous_segment = {
        'start_frame': None,
        'end_frame': None,
        'bright_frame_num': None,
        'qr_code': None,
        'validated': False
    }
    
    # IMPORTANT: Reset flash calibration for each video (different lighting conditions)
    state_flash_calibration_counter = 0
    state_flash_max_brightness_seen = 0
    state_flash_brightness_threshold = 180  # Will be recalibrated in first 10 frames
    
    # DON'T reset: state_used_takes, state_current_roll
    
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
    print(f"\n=== End of video - checking for pending segments ===")
    print(f"state_previous_segment: {state_previous_segment}")
    print(f"state_slate_detected: {state_slate_detected}, state_bright_frame_detected: {state_bright_frame_detected}")
    
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
    else:
        print(f"Case 1 not matched - no previous segment to finalize")
    
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
    else:
        print(f"Case 2 not matched - no current active segment to finalize")
    
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
    print(f"\nLade JSON-Metadaten um bereits verarbeitete Takes zu markieren...")
    if load_json_metadata():
        # Detect rolls for ALL videos before start_counter
        # Count how many videos per roll were processed
        roll_video_count = {}
        
        for i in range(start_counter):
            video_path = video_files_sorted[i][1]
            roll = detect_roll_from_video(video_path, state_json_meta)
            if roll:
                roll_video_count[roll] = roll_video_count.get(roll, 0) + 1
        
        print(f"  Videos vor Start: {roll_video_count}")
        
        # For each roll, mark the first N takes as used (where N = number of videos for that roll)
        for roll, video_count in roll_video_count.items():
            all_takes = []
            for shot in state_json_meta.get("shotlist", []):
                for take in shot.get("takes", []):
                    if take.get("roll") == roll:
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
            
            # Mark first N takes as used for this roll
            takes_to_mark = min(video_count, len(sorted_takes))
            for i in range(takes_to_mark):
                take = sorted_takes[i]
                take_key = f"{take['scene']}_{take['shot']}_{take['take']}_{take['roll']}"
                state_used_takes.append(take_key)
            
            print(f"  Roll {roll}: {takes_to_mark} Takes als verwendet markiert")

print(f"\nProcessing {len(video_files_sorted) - start_counter} video files...")

for idx, (creation_time, video_path) in enumerate(video_files_sorted):
    if idx < start_counter:
        continue  # Skip videos before start_counter
    
    print(f"\n--- Verarbeitung von Video {idx + 1}/{len(video_files_sorted)}: {video_path} (created: {datetime.fromtimestamp(creation_time)}) ---")
    process_video(video_path, True)

print(f"\n=== All videos processed. Total takes used: {len(state_used_takes)} ===")
