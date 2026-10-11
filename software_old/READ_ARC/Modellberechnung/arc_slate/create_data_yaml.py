import os

# Zum Skript-Verzeichnis wechseln
script_dir = os.path.dirname(os.path.abspath(__file__))
os.chdir(script_dir)

# data.yaml erstellen
classes = ["ARC-SLATE"]

yaml_content = f"""# YOLO Daten-Konfiguration
path: {os.path.abspath('.')}
train: images
val: images

# Klassen
names:
  0: ARC-SLATE
"""

with open('data.yaml', 'w', encoding='utf-8') as f:
    f.write(yaml_content)

print("✓ data.yaml erstellt mit 1 Klasse:")
print("  0: ARC-SLATE")