from ultralytics import YOLO
import os
from pathlib import Path
import torch

# Zum Skript-Verzeichnis wechseln
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

print("="*70)
print("🚀 YOLO Segmentation Training für Label Studio")
print("="*70)

# Device-Auswahl: CUDA, MPS (Apple Silicon), CPU
if torch.cuda.is_available():
    device = '0'
    print(f"\n✅ NVIDIA GPU verfügbar: {torch.cuda.get_device_name(0)}")
elif hasattr(torch.backends, "mps") and torch.backends.mps.is_available():
    device = 'mps'
    print(f"\n🍏 Apple Silicon GPU (MPS) erkannt und wird genutzt!")
    print(f"   (Training wird schneller als auf CPU, aber langsamer als CUDA sein)")
else:
    device = 'cpu'
    print(f"\n💻 Keine GPU gefunden, nutze CPU")
    print(f"   (Training wird länger dauern: ~20-40 Minuten)")

# Überprüfungen
if not os.path.exists('data.yaml'):
    print("\n❌ data.yaml nicht gefunden!")
    print("   Führen Sie zuerst aus: python prepare_training.py")
    exit(1)

if not os.path.exists('images') or not os.path.exists('labels'):
    print("\n❌ 'images' und 'labels' Ordner müssen existieren!")
    exit(1)

# Anzahl Bilder
num_images = len(list(Path('images').glob('*.jpg'))) + \
             len(list(Path('images').glob('*.png'))) + \
             len(list(Path('images').glob('*.jpeg')))

num_labels = len(list(Path('labels').glob('*.txt')))

print(f"\n📊 Datensatz:")
print(f"   Bilder: {num_images}")
print(f"   Labels: {num_labels}")

if num_images < 10:
    print("\n⚠️  WARNUNG: Sehr wenig Trainingsdaten!")
    print("   Mit 35 Bildern sollte es funktionieren, aber mehr ist besser.")
    response = input("\n   Trotzdem fortfahren? (j/n): ")
    if response.lower() != 'j':
        exit(0)

# Modell-Auswahl
print(f"\n🤖 Wähle YOLO-Modell:")
print(f"   1. yolov8n-seg.pt  (Nano  - EMPFOHLEN für CPU)")
print(f"   2. yolov8s-seg.pt  (Small - langsamer auf CPU)")

choice = input("\n   Auswahl (Standard: 1): ").strip() or "1"

model_map = {
    "1": "yolov8n-seg.pt",
    "2": "yolov8s-seg.pt",
}

model_name = model_map.get(choice, "yolov8n-seg.pt")
print(f"\n📦 Lade Modell: {model_name}")

model = YOLO(model_name)

# Training-Parameter
print(f"\n{'='*70}")
print(f"🏋️  Starte Training auf {device.upper()}...")
print(f"{'='*70}")

if device == 'cpu':
    print(f"   ⏱️  Geschätzte Dauer: 20-40 Minuten")
    print(f"   💡 Sie können in der Zwischenzeit weitermachen")
else:
    print(f"   ⏱️  Geschätzte Dauer: 5-15 Minuten")

print(f"{'='*70}\n")

results = model.train(
    data='data.yaml',
    
    # Epochen - reduziert für CPU
    epochs=100 if device == 'cpu' else 150,
    patience=30,
    
    # Bildgröße und Batch - angepasst für CPU
    imgsz=640,
    batch=2 if device == 'cpu' else 4,  # Kleinere Batch für CPU
    
    # Optimierung
    optimizer='AdamW',
    lr0=0.001,
    lrf=0.01,
    momentum=0.937,
    weight_decay=0.0005,
    
    # Augmentation
    augment=True,
    hsv_h=0.015,
    hsv_s=0.7,
    hsv_v=0.4,
    degrees=10,
    translate=0.1,
    scale=0.5,
    shear=0.0,
    perspective=0.0,
    flipud=0.0,
    fliplr=0.5,
    mosaic=1.0,
    mixup=0.1,
    copy_paste=0.1,
    
    # Speichern
    project='runs/segment',
    name='label_studio_model',
    exist_ok=True,
    save=True,
    save_period=50,
    
    # Validation
    val=True,
    plots=True,
    
    # Performance
    pretrained=True,
    verbose=True,
    device=device,
    workers=2 if device == 'cpu' else 8,  # Weniger Worker für CPU
)

print(f"\n{'='*70}")
print(f"✅ Training abgeschlossen!")
print(f"{'='*70}")

# Modell-Pfad
best_model = Path('runs/segment/label_studio_model/weights/best.pt')

if best_model.exists():
    size_mb = best_model.stat().st_size / (1024*1024)
    print(f"\n📦 Bestes Modell:")
    print(f"   Pfad: {best_model}")
    print(f"   Größe: {size_mb:.1f} MB")
    
    print(f"\n📊 Trainings-Ergebnisse:")
    print(f"   Ordner: runs/segment/label_studio_model/")
    print(f"   - weights/best.pt  (bestes Modell)")
    print(f"   - results.png      (Trainings-Kurven)")
    print(f"   - confusion_matrix.png")
    
    print(f"\n🔄 Nächste Schritte:")
    print(f"   1. ML-Backend neu starten:")
    print(f"      python start_ml_backend.py")
    print(f"   2. In Label Studio neuen Task öffnen")
    print(f"   3. Jetzt sollten IHRE Klassen erkannt werden!")
    
else:
    print(f"\n⚠️  Modell nicht gefunden. Überprüfen Sie Fehler oben.")

print(f"{'='*70}\n")