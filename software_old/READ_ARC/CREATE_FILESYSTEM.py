import os
import shutil
from pathlib import Path

def create_filesystem(location):
    base_path = Path(location)
    subfolders = ["01_IMPORT", "02_JSONFILE", "03_EXPORT", "04_CSVFILE"]
    for folder in subfolders:
        path = base_path / folder
        path.mkdir(parents=True, exist_ok=True)

    # Dateien sortieren
    for file in base_path.iterdir():
        if file.is_file():
            if file.suffix.lower() == ".mov":
                shutil.move(str(file), str(base_path / "01_IMPORT" / file.name))
            elif file.suffix.lower() == ".json":
                shutil.move(str(file), str(base_path / "02_JSONFILE" / file.name))
                
create_filesystem("/Users/julian/Desktop/test_filesystem")