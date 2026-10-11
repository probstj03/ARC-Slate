import os
from pathlib import Path

# Zum Skript-Verzeichnis wechseln
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

print("🔍 Überprüfe Trainings-Daten...\n")

# 1. Klassen (direkt definiert)
classes = ["ARC-SLATE"]
print(f"✅ {len(classes)} Klasse(n) definiert:")
for i, cls in enumerate(classes):
    print(f"   {i}: {cls}")

# 2. Bilder zählen
images_dir = Path('images')
if not images_dir.exists():
    print(f"\n❌ Ordner 'images/' nicht gefunden!")
    exit(1)

image_files = list(images_dir.glob('*.jpg')) + list(images_dir.glob('*.png')) + list(images_dir.glob('*.jpeg'))
print(f"\n✅ {len(image_files)} Bilder in images/")

# 3. Labels zählen
labels_dir = Path('labels')
if not labels_dir.exists():
    print(f"❌ Ordner 'labels/' nicht gefunden!")
    exit(1)

label_files = list(labels_dir.glob('*.txt'))
print(f"✅ {len(label_files)} Label-Dateien in labels/")

# 4. Überprüfe Übereinstimmung
matched = 0
unmatched_images = []
unmatched_labels = []

for img in image_files:
    label_file = labels_dir / f"{img.stem}.txt"
    if label_file.exists():
        matched += 1
    else:
        unmatched_images.append(img.name)

for lbl in label_files:
    has_image = False
    for ext in ['.jpg', '.png', '.jpeg']:
        if (images_dir / f"{lbl.stem}{ext}").exists():
            has_image = True
            break
    if not has_image:
        unmatched_labels.append(lbl.name)

print(f"\n📊 Übereinstimmung: {matched} Bild-Label-Paare")

if unmatched_images:
    print(f"⚠️  {len(unmatched_images)} Bilder ohne Labels:")
    for img in unmatched_images[:5]:
        print(f"   - {img}")
    if len(unmatched_images) > 5:
        print(f"   ... und {len(unmatched_images) - 5} weitere")

if unmatched_labels:
    print(f"⚠️  {len(unmatched_labels)} Labels ohne Bilder:")
    for lbl in unmatched_labels[:5]:
        print(f"   - {lbl}")

# 5. data.yaml erstellen
print(f"\n📝 Erstelle data.yaml...")

yaml_content = f"""# YOLO Segmentation Training Configuration
# Generiert am: 2025-11-20

# Pfade (absolut für Sicherheit)
path: {Path('.').absolute()}
train: images
val: images  # Nutzen Sie erstmal alle Bilder für Training + Validation

# Anzahl Klassen
nc: {len(classes)}

# Klassennamen
names:
"""

for i, cls in enumerate(classes):
    yaml_content += f"  {i}: {cls}\n"

with open('data.yaml', 'w', encoding='utf-8') as f:
    f.write(yaml_content)

print(f"✅ data.yaml erstellt")
print(f"\n{'='*60}")
print(f"📊 ZUSAMMENFASSUNG:")
print(f"{'='*60}")
print(f"Klassen:        {len(classes)}")
print(f"Bilder:         {len(image_files)}")
print(f"Labels:         {len(label_files)}")
print(f"Matched Paare:  {matched}")
print(f"{'='*60}")

if matched >= 10:
    print(f"\n✅ Genug Daten für Training!")
    print(f"   Nächster Schritt: python train_model.py")
else:
    print(f"\n⚠️  Nur {matched} Trainingsbeispiele!")
    print(f"   Minimum: ~10-20 für erste Ergebnisse")
    print(f"   Empfohlen: 50+ für gute Ergebnisse")