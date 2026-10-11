"""
Klappe Detector Modul - Kann in anderen Projekten importiert werden
"""

from ultralytics import YOLO
from pathlib import Path
import cv2
import numpy as np

class SlateDetector:
    def __init__(self, model_path='slate_detection/runs/detect/label_studio_model/weights/best.pt',
                 segment_model_path='slate_detection/runs/segment/label_studio_model/weights/best.pt',
                 classes_file='slate_detection/classes.txt',
                 use_segmentation=False):
        """
        Initialisiert den Detektor
        
        Args:
            model_path: Pfad zum trainierten Detection-Modell
            segment_model_path: Pfad zum trainierten Segmentation-Modell
            classes_file: Pfad zur classes.txt
            use_segmentation: Wenn True, wird das Segmentation-Modell verwendet
        """
        self.use_segmentation = use_segmentation
        
        try:
            if use_segmentation:
                self.model = YOLO(segment_model_path)
                print(f"✅ Segmentation-Modell geladen: {segment_model_path}")
            else:
                self.model = YOLO(model_path)
                print(f"✅ Detection-Modell geladen: {model_path}")
        except Exception as e:
            print(f"Fehler beim Laden des YOLO-Modells: {model_path if not use_segmentation else segment_model_path}")
            print("Stelle sicher, dass die Datei existiert und nicht beschädigt ist.")
            print(f"Fehlerdetails: {e}")
            raise

        # Klassen laden
        with open(classes_file, 'r', encoding='utf-8') as f:
            self.class_names = [line.strip() for line in f.readlines()]
        
        print(f"✅ KlappeDetector initialisiert mit {len(self.class_names)} Klassen")
    
    def detect(self, image, conf_threshold=0.25):
        """
        Erkennt Klappen im Bild
        
        Args:
            image: Bild als Pfad (str/Path) oder numpy array
            conf_threshold: Confidence-Schwellwert
            
        Returns:
            Liste von Dictionaries mit Erkennungen
        """
        results = self.model.predict(image, conf=conf_threshold, verbose=False)
        result = results[0]
        
        detections = []
        
        if result.boxes is not None:
            for box in result.boxes:
                x1, y1, x2, y2 = box.xyxy[0].tolist()
                class_idx = int(box.cls)
                
                detections.append({
                    'class': self.class_names[class_idx],
                    'confidence': float(box.conf),
                    'bbox': {
                        'x1': int(x1),
                        'y1': int(y1),
                        'x2': int(x2),
                        'y2': int(y2)
                    }
                })
        
        return detections
    
    def has_slate(self, image, slate_class='ARC-SLATE', min_confidence=0.5):
        """
        Prüft ob eine Klappe im Bild ist
        
        Args:
            image: Bild
            klappe_class: Name der Klappe-Klasse
            min_confidence: Minimale Confidence
            
        Returns:
            True wenn Klappe gefunden, sonst False
        """
        detections = self.detect(image, conf_threshold=min_confidence)
        
        for det in detections:
            if det['class'] == slate_class:
                return True
        
        return False
    
    def get_slate_position(self, image, slate_class='ARC-SLATE'):
        """
        Gibt Position der ersten gefundenen Klappe zurück
        
        Returns:
            Dictionary mit bbox oder None
        """
        detections = self.detect(image)
        
        for det in detections:
            if det['class'] == slate_class:
                return det['bbox']
        
        return None
    
    def get_slate_polygon(self, image, slate_class='ARC-SLATE'):
        """
        Gibt ein 4-Punkte-Polygon der Slate zurück (nur für Segmentation-Modell)
        
        Args:
            image: Bild
            slate_class: Name der Slate-Klasse
            
        Returns:
            numpy array mit 4 Punkten [[x1,y1], [x2,y2], [x3,y3], [x4,y4]] oder None
        """
        if not self.use_segmentation:
            print("Warnung: get_slate_polygon() funktioniert nur mit dem Segmentation-Modell!")
            return None
        
        # Bild laden falls Pfad
        if isinstance(image, (str, Path)):
            image = cv2.imread(str(image))
        
        h, w = image.shape[:2]
        
        results = self.model.predict(image, conf=0.25, verbose=False)
        result = results[0]
        
        if result.masks is not None and len(result.masks) > 0:
            for i, mask in enumerate(result.masks):
                if result.boxes is not None and i < len(result.boxes):
                    class_idx = int(result.boxes[i].cls)
                    if self.class_names[class_idx] == slate_class:
                        # Extrahiere die Maske und skaliere auf Bildgröße
                        mask_data = mask.data[0].cpu().numpy()
                        
                        # Skaliere Maske auf die Original-Bildgröße
                        mask_h, mask_w = mask_data.shape
                        if mask_h != h or mask_w != w:
                            mask_data = cv2.resize(mask_data, (w, h), interpolation=cv2.INTER_LINEAR)
                        
                        # Konvertiere zu binärer Maske
                        mask_binary = (mask_data > 0.5).astype(np.uint8) * 255
                        
                        # Finde Kontur
                        contours, _ = cv2.findContours(
                            mask_binary,
                            cv2.RETR_EXTERNAL,
                            cv2.CHAIN_APPROX_SIMPLE
                        )
                        
                        if len(contours) > 0:
                            # Größte Kontur nehmen
                            cnt = max(contours, key=cv2.contourArea)
                            
                            # Verwende konvexe Hülle für bessere Eckpunkte
                            hull = cv2.convexHull(cnt)
                            
                            # Eckpunkte approximieren (4 Punkte)
                            epsilon = 0.02 * cv2.arcLength(hull, True)
                            approx = cv2.approxPolyDP(hull, epsilon, True)
                            
                            # Falls nicht 4 Punkte, versuche mit größerer Toleranz
                            if len(approx) < 4:
                                epsilon = 0.05 * cv2.arcLength(hull, True)
                                approx = cv2.approxPolyDP(hull, epsilon, True)
                            
                            # Falls immer noch nicht 4, nimm die 4 äußersten Punkte
                            if len(approx) < 4:
                                # Nimm die 4 Punkte mit den meisten Abstand zum Zentrum
                                moments = cv2.moments(hull)
                                cx = int(moments['m10'] / moments['m00']) if moments['m00'] != 0 else w // 2
                                cy = int(moments['m01'] / moments['m00']) if moments['m00'] != 0 else h // 2
                                
                                distances = np.sqrt((hull[:, 0, 0] - cx)**2 + (hull[:, 0, 1] - cy)**2)
                                top_indices = np.argsort(distances)[-4:]
                                approx = hull[top_indices]
                            
                            # Stelle sicher, dass genau 4 Punkte vorhanden sind
                            if len(approx) >= 4:
                                # Nimm erste 4 Punkte
                                points = approx[:4].reshape(4, 2).astype(np.float32)
                                polygon = self._order_points(points)
                                return polygon
        
        return None
    
    @staticmethod
    def _order_points(points):
        """
        Sortiert 4 Punkte im Uhrzeigersinn: oben-links, oben-rechts, unten-rechts, unten-links
        """
        # Berechne Mittelpunkt
        center = points.mean(axis=0)
        
        # Sortiere nach Winkel vom Mittelpunkt
        angles = np.arctan2(points[:, 1] - center[1], points[:, 0] - center[0])
        sorted_indices = np.argsort(angles)
        
        return points[sorted_indices]
    
    def draw_detections(self, image, detections=None, conf_threshold=0.25):
        """
        Zeichnet Erkennungen auf das Bild
        
        Args:
            image: Bild (wird modifiziert!)
            detections: Optional, sonst wird detect() aufgerufen
            
        Returns:
            Annotiertes Bild
        """
        if detections is None:
            detections = self.detect(image, conf_threshold)
        
        # Bild laden falls Pfad
        if isinstance(image, (str, Path)):
            img = cv2.imread(str(image))
        else:
            img = image.copy()
        
        for det in detections:
            bbox = det['bbox']
            
            # Box
            cv2.rectangle(
                img,
                (bbox['x1'], bbox['y1']),
                (bbox['x2'], bbox['y2']),
                (0, 255, 0), 2
            )
            
            # Label
            label = f"{det['class']}: {det['confidence']:.2f}"
            cv2.putText(
                img, label,
                (bbox['x1'], bbox['y1'] - 10),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6,
                (0, 255, 0), 2
            )
        
        return img


# Beispiel-Nutzung in anderem Script:
if __name__ == '__main__':
    # Detektor initialisieren
    detector = SlateDetector()
    
    # Bild testen
    test_image = 'images/test.jpg'
    
    if Path(test_image).exists():
        # Erkennungen durchführen
        detections = detector.detect(test_image)
        print(f"✅ {len(detections)} Objekt(e) erkannt")
        
        # Prüfen ob Klappe vorhanden
        if detector.has_klappe(test_image):
            print("🎯 Klappe gefunden!")
            position = detector.get_klappe_position(test_image)
            print(f"   Position: {position}")
        
        # Visualisieren
        annotated = detector.draw_detections(test_image, detections)
        cv2.imshow('Ergebnis', annotated)
        cv2.waitKey(0)