import cv2
import sys
import json
import subprocess
from pathlib import Path
import os
import glob
import csv
from datetime import datetime

project_name = "test_01"

current_code = None
current_roll = None
current_scene = None
current_shot = None
current_take = None

project_location = "/Users/julian/Library/CloudStorage/OneDrive-HAW-HH/Projekte/Digitale Filmklappe/Software/READ_ARC/test_01/"


def create_filesystem(name, location):

    base_path = name + location
    base_path = Path(base_path)
    subfolders = ["01_IMPORT", "02_JSONFILE", "03_EXPORT", "04_CSVFILE"]
    for folder in subfolders:
        path = base_path / folder
        path.mkdir(parents=True, exist_ok=True)
    print(f"Filesystem created for project: {name}")
    
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

def read_qr_from_video(video):
    qr_detector = cv2.QRCodeDetector()
    frame_count = 0
    found = False

    while True:
        ret, frame = video.read()
        if not ret:
            break  # End of video

        frame_count += 1
        # Try to detect and decode QR code
        data, bbox, _ = qr_detector.detectAndDecode(frame)
        if data:
            print(f"QR Code found in frame {frame_count}: {data}")
            found = True
            return data

    video.release()
    if not found:
        print("No QR code found in the video.")

def get_video_frame_count(video):
    """
    Returns the total number of frames in the video.
    """
    frame_count = int(video.get(cv2.CAP_PROP_FRAME_COUNT))
    return frame_count

def decode_qr_code(qr_code_data):
    global current_code, current_roll, current_scene, current_shot, current_take
    if qr_code_data:
        parts = qr_code_data.split('_')
        if len(parts) == 5:
            current_code, current_roll, current_scene, current_shot, current_take = parts
            print(f"Code: {current_code}, Roll: {current_roll}, Scene: {current_scene}, Shot: {current_shot}, Take: {current_take}")
        else:
            print("QR code data format is incorrect.")
    else:
        print("No QR code data to decode.")
       
def detect_bright_frame(video, brightness_threshold=230, area_ratio=1, roi_top=440, roi_left=720, roi_height=20, roi_width=20):
    """
    Detects the first frame in the video where a large area is completely bright (blown out) in a specific region.
    Optionally displays each frame with the ROI rectangle.
    Returns the frame number (starting from 1) or None if not found.
    """
    frame_num = 0
    while True:
        ret, frame = video.read()
        if not ret:
            print(f"End of video or cannot read frame at frame {frame_num+1}")
            break
        frame_num += 1
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        h, w = gray.shape
        y1 = roi_top
        x1 = roi_left
        y2 = y1 + (roi_height if roi_height is not None else h - y1)
        x2 = x1 + (roi_width if roi_width is not None else w - x1)
        if y1 < 0 or x1 < 0 or y2 > h or x2 > w or y2 <= y1 or x2 <= x1:
            print(f"Invalid ROI for frame {frame_num}, skipping.")
            continue
        roi = gray[y1:y2, x1:x2]
        bright_pixels = (roi >= brightness_threshold).sum()
        total_pixels = roi.size
        ratio = bright_pixels / total_pixels if total_pixels > 0 else 0
        if total_pixels > 0 and ratio >= area_ratio:
            print(f"Bright frame detected at frame {frame_num}")
            video.release()
            return frame_num

    print("No bright frame detected.")
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

def open_json_file(folder_path):
    try:
        json_folder = os.path.join(folder_path, "02_JSONFILE")
        json_files = sorted(glob.glob(os.path.join(json_folder, "*.json")))
        if not json_files:
            print("No JSON files found in 02_JSONFILE folder.")
            return None
        with open(json_files[0], 'r') as f:
            data = json.load(f)
        return data
    except Exception as e:
        print(f"Error opening JSON file: {e}")
        return None

def add_metadata(location, qr_code_data, sync_frame, framecount):
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
    except Exception as e:
        print(f"Error opening JSON file: {e}")
        return



    # --- Timecode einbetten ---
    sync_timecode = next(
        (take["timecode"]
        for shot in json_meta["shotlist"] if shot.get("shot") == current_shot
        for take in shot.get("takes", []) if take.get("take") == int(current_take)),
        "00:00:00:00" 
    )
    
    framerate = next(
        (shot["framerate"]
        for shot in json_meta["shotlist"] if shot.get("shot") == current_shot),
        24
    )

    print("Sync TC: " + sync_timecode)
    start_timecode = adjust_timecode(sync_timecode, -sync_frame, framerate)
    print("Start TC: " + start_timecode)

    input_mov = os.path.join(location, "01_IMPORT", f"{project_name}.mov")
    output_mov = os.path.join(location, "03_EXPORT", current_code, "SCENE_" + current_scene, "SHOT_" + current_shot, f"{current_code}_{current_roll}_{current_scene}_{current_shot}_TAKE_{current_take}.mov")
    output_dir = os.path.dirname(output_mov)
    os.makedirs(output_dir, exist_ok=True)

    # Aufnahme-Datum und -Zeit aus JSON holen (oder Systemzeit als Fallback)
    recording_date = None
    for shot in json_meta.get("shotlist", []):
        if shot.get("shot") == current_shot:
            for take in shot.get("takes", []):
                if take.get("take") == int(current_take):
                    date_str = take.get("recording_date")
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
        raise RuntimeError("ffmpeg fehlgeschlagen")
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
        "Director",
        "DOP"
    ]

    director = json_meta.get("director", "")
    dop = json_meta.get("dop", "")

    with open(csv_file_path, 'w', newline='') as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fieldnames)
        writer.writeheader()

        # Suche nur den aktuellen Shot und Take
        for shot in json_meta.get("shotlist", []):
            scene = shot.get("scene", "")
            shot_num = shot.get("shot", "")
            daynight = shot.get("daynight", "")
            environment = shot.get("environment", "")

            if shot_num == current_shot:
                for take in shot.get("takes", []):
                    take_num = str(take.get("take", ""))
                    roll = take.get("roll", "")
                    if take_num == str(current_take):
                        file_name = f"{current_code}_{roll}_{scene}_{shot_num}_TAKE_{take_num}.mov"
                        clip_dir = f"{location}03_EXPORT/{current_code}/SCENE_{scene}/SHOT_{shot_num}"
                        date = take.get("date", "")

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
                            "Director": director,
                            "DOP": dop,
                        }

                        writer.writerow(row)
                        print(f"CSV file created at {csv_file_path}")
                        return 

# --- Hauptablauf ---
video_file = open_video_file(project_location)
qr_code_data = read_qr_from_video(video_file)
framecount = get_video_frame_count(video_file)
sync_frame = detect_bright_frame(video_file)
add_metadata(project_location, qr_code_data, sync_frame, framecount)

