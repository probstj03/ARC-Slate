from ultralytics import YOLO
import os
from pathlib import Path

print("="*70)
print("🚀 YOLO Detection Training (Bounding Boxes)")
print("="*70)

# Überprüfungen
if not os.path.exists('data.yaml'):
    print("\n❌ data.yaml nicht gefunden!")
    exit(1)

# Anzahl Bilder
num_images = len(list(Path('images').glob('*.jpg'))) + \
             len(list(Path('images').glob('*.png'))) + \
             len(list(Path('images').glob('*.jpeg')))

num_labels = len(list(Path('labels').glob('*.txt')))

print(f"\n📊 Datensatz:")
print(f"   Bilder: {num_images}")
print(f"   Labels: {num_labels}")

# WICHTIG: Detection-Modell laden (NICHT -seg!)
print(f"\n📦 Lade Detection-Modell: yolov8n.pt")
model = YOLO('yolov8n.pt')  # Detection, nicht Segmentation!

print(f"\n{'='*70}")
print(f"🏋️  Starte Training für Bounding Box Detection...")
print(f"{'='*70}\n")

results = model.train(
    data='data.yaml',
    
    # Epochen
    epochs=150,
    patience=50,
    
    # Bildgröße und Batch
    imgsz=640,
    batch=8,  # Kann größer sein als bei Segmentation
    
    # Optimierung
    optimizer='AdamW',
    lr0=0.001,
    
    # Augmentation
    augment=True,
    hsv_h=0.015,
    hsv_s=0.7,
    hsv_v=0.4,
    degrees=10,
    translate=0.1,
    scale=0.5,
    flipud=0.0,
    fliplr=0.5,
    mosaic=1.0,
    mixup=0.1,
    
    # Speichern (beachte: detect statt segment!)
    project='runs/detect',  # Geändert!
    name='label_studio_model',
    exist_ok=True,
    save=True,
    
    # Validation
    val=True,
    plots=True,
    
    # Performance
    pretrained=True,
    verbose=True,
    device='cpu',  # oder 0 für GPU
)

print(f"\n{'='*70}")
print(f"✅ Training abgeschlossen!")
print(f"{'='*70}")

best_model = Path('runs/detect/label_studio_model/weights/best.pt')

if best_model.exists():
    size_mb = best_model.stat().st_size / (1024*1024)
    print(f"\n📦 Bestes Modell:")
    print(f"   Pfad: {best_model}")
    print(f"   Größe: {size_mb:.1f} MB")
    
    print(f"\n🔄 Nächste Schritte:")
    print(f"   1. ML-Backend neu starten")
    print(f"   2. In Label Studio: Settings → Labeling Interface auf RectangleLabels ändern")
    print(f"   3. Neuen Task öffnen → Bounding Boxes sollten erscheinen!")
    
print(f"{'='*70}\n")