from label_studio_ml.model import LabelStudioMLBase
from label_studio_ml.utils import get_local_path
from ultralytics import YOLO
import os
import logging
from pathlib import Path
import cv2
import numpy as np

logger = logging.getLogger(__name__)

class YOLODetectionModel(LabelStudioMLBase):
    def __init__(self, **kwargs):
        super(YOLODetectionModel, self).__init__(**kwargs)
        
        try:
            # Klassen direkt definiert
            self.class_names = ["ARC-SLATE"]
            logger.info(f"✅ Klassen definiert: {', '.join(self.class_names)}")
            
            # Zum Skript-Verzeichnis wechseln für korrekten Pfad
            script_dir = os.path.dirname(os.path.abspath(__file__))
            os.chdir(script_dir)
            logger.info(f"📁 Arbeitsverzeichnis: {os.getcwd()}")
            
            # Pfade zu möglichen Modellen (mit absoluten Pfaden)
            preferred_paths = [
                kwargs.get('model_path'),
                os.environ.get('YOLO_MODEL_PATH'),
                os.path.join(script_dir, 'runs', 'segment', 'label_studio_model', 'weights', 'best.pt'),
                os.path.join(script_dir, 'yolov8n-seg.pt'),
                'runs/segment/label_studio_model/weights/best.pt',
                'yolov8n-seg.pt',
            ]

            model_path = None
            for path in preferred_paths:
                if path:
                    logger.info(f"🔍 Überprüfe: {path}")
                    if os.path.exists(path):
                        model_path = path
                        logger.info(f"✅ Gefunden: {path}")
                        break

            if model_path:
                logger.info(f"📦 Lade Segmentationsmodell: {model_path}")
                self.model = YOLO(model_path)
                logger.info(f"✅ Modell erfolgreich geladen!")
            else:
                logger.error(f"❌ Kein Segmentationsmodell gefunden! Verfügbare Pfade: {preferred_paths}")
                raise FileNotFoundError("Segmentationsmodell nicht gefunden")
            
            logger.info(f"🚀 Modell bereit mit Klassen: {', '.join(self.class_names)}")
            
            # Label Studio Datenverzeichnis
            self.label_studio_host = kwargs.get('hostname', 'http://localhost:8080')
            self.label_studio_access_token = kwargs.get('access_token', '')
        
        except Exception as e:
            logger.error(f"❌ Fehler beim Initialisieren des Modells: {e}", exc_info=True)
            raise
        
    def setup(self):
        """Setup für Label Studio Backend"""
        logger.info("🔧 Setup aufgerufen")
        return {
            'model_version': 'yolov8-segment',
            'class_labels': self.class_names
        }
        
    def _get_image_path(self, task):
        """Ermittelt den korrekten lokalen Pfad zum Bild"""
        image_url = task['data'].get('image', '')
        
        logger.info(f"🔍 Original URL: {image_url}")
        
        # Verschiedene Pfad-Formate die Label Studio nutzen kann
        possible_paths = []
        
        # 1. Direkter Pfad
        if os.path.exists(image_url):
            logger.info(f"✅ Direkter Pfad gefunden: {image_url}")
            return image_url
        
        # 2. Relativer Pfad zu images/
        image_name = Path(image_url).name
        local_path = os.path.join('images', image_name)
        if os.path.exists(local_path):
            logger.info(f"✅ Gefunden in images/: {local_path}")
            return local_path
        
        # 3. Label Studio Upload-Verzeichnis
        home = Path.home()
        media_dirs = [
            home / '.local' / 'share' / 'label-studio' / 'media' / 'upload',
            home / 'label-studio' / 'media' / 'upload',
            Path('media') / 'upload',
        ]
        
        for media_dir in media_dirs:
            full_path = media_dir / image_name
            if full_path.exists():
                logger.info(f"✅ Gefunden in Label Studio media: {full_path}")
                return str(full_path)
        
        # 4. URL-basierter Pfad
        if '/data/upload/' in image_url:
            filename = image_url.split('/data/upload/')[-1]
            for media_dir in media_dirs:
                full_path = media_dir / filename
                if full_path.exists():
                    logger.info(f"✅ Gefunden via /data/upload/: {full_path}")
                    return str(full_path)
        
        # 5. Versuche get_local_path
        try:
            path = get_local_path(
                image_url,
                hostname=self.label_studio_host,
                access_token=self.label_studio_access_token
            )
            if path and os.path.exists(path):
                logger.info(f"✅ Gefunden via get_local_path: {path}")
                return path
        except Exception as e:
            logger.warning(f"get_local_path fehlgeschlagen: {e}")
        
        logger.error(f"❌ Bild nicht gefunden: {image_url}")
        raise FileNotFoundError(f"Bild nicht gefunden: {image_url}")
        
    def predict(self, tasks, **kwargs):
        logger.info(f"\n{'='*60}")
        logger.info(f"🎯 PREDICT aufgerufen mit {len(tasks)} task(s)")
        logger.info(f"{'='*60}")
        
        predictions = []
        
        for idx, task in enumerate(tasks):
            logger.info(f"\n📋 Task {idx+1}/{len(tasks)}: ID={task.get('id', 'N/A')}")
            
            try:
                # Bildpfad ermitteln
                image_path = self._get_image_path(task)
                logger.info(f"📸 Verarbeite: {image_path}")
                
                # Vorhersage durchführen (Segmentation)
                results = self.model.predict(
                    image_path,
                    conf=0.25,
                    iou=0.7,
                    verbose=False
                )
                
                result = results[0]
                
                # Bildabmessungen
                img_width = result.orig_shape[1]
                img_height = result.orig_shape[0]
                logger.info(f"📐 Bildgröße: {img_width}x{img_height}")
                logger.info(f"📐 Masken-Shape: {masks_np.shape if 'masks_np' in locals() else 'N/A'}")
                
                task_predictions = []
                
                # Segmentation: mask → contour → polygon
                masks = None
                try:
                    # ultralytics result.masks.data is often the per-instance mask array
                    masks = getattr(result.masks, 'data', result.masks)
                except Exception:
                    masks = None

                if masks is not None:
                    # normalize masks to numpy array of shape (N, H, W)
                    try:
                        # if torch tensor
                        import torch
                        if isinstance(masks, torch.Tensor):
                            masks_np = masks.cpu().numpy()
                        else:
                            masks_np = np.array(masks)
                    except Exception:
                        masks_np = np.array(masks)

                    # Stelle sicher, dass Masken die gleiche Größe wie das Original-Bild haben
                    if masks_np.size > 0:
                        # Wenn Masken kleiner sind als das Bild, resize sie
                        mask_h, mask_w = masks_np.shape[-2:]
                        if mask_h != img_height or mask_w != img_width:
                            logger.info(f"🔧 Resize Masken von {mask_w}x{mask_h} zu {img_width}x{img_height}")
                            resized_masks = []
                            for mask in masks_np:
                                resized = cv2.resize(mask.astype('float32'), (img_width, img_height))
                                resized_masks.append(resized)
                            masks_np = np.array(resized_masks)

                    if masks_np.size == 0:
                        logger.info("ℹ️  Keine Masken erkannt")
                    else:
                        num_objs = masks_np.shape[0]
                        logger.info(f"✅ {num_objs} Maske(n) erkannt")

                        for obj_idx in range(num_objs):
                            mask = masks_np[obj_idx]
                            # mask may be float in [0..1] or bool
                            bin_mask = (mask > 0.5).astype('uint8') * 255

                            # find contours
                            contours, _ = cv2.findContours(bin_mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
                            # try to find class/confidence from corresponding box if available
                            class_idx = 0
                            confidence = 0.0
                            try:
                                if result.boxes is not None and len(result.boxes) > obj_idx:
                                    box = result.boxes[obj_idx]
                                    class_idx = int(box.cls)
                                    confidence = float(box.conf)
                            except Exception:
                                pass

                            if class_idx < len(self.class_names):
                                class_name = self.class_names[class_idx]
                            else:
                                class_name = result.names.get(class_idx, f"class_{class_idx}")

                            for cnt in contours:
                                if cv2.contourArea(cnt) < 10:
                                    continue
                                epsilon = 0.01 * cv2.arcLength(cnt, True)
                                approx = cv2.approxPolyDP(cnt, epsilon, True)
                                pts = approx.squeeze()
                                if pts.ndim != 2 or pts.shape[0] < 3:
                                    continue
                                # convert to percent (0-100) consistent with repo boxes
                                points_pct = [[(float(x) / img_width) * 100, (float(y) / img_height) * 100] for x, y in pts]

                                task_predictions.append({
                                    'from_name': 'label',
                                    'to_name': 'image',
                                    'type': 'polygonlabels',
                                    'value': {
                                        'points': points_pct,
                                        'polygonlabels': [class_name]
                                    },
                                    'score': confidence
                                })
                else:
                    logger.info("ℹ️  Keine Masken/Segmentationsergebnisse gefunden")
                
                predictions.append({
                    'result': task_predictions,
                    'score': 0.0,
                    'model_version': 'yolov8-detect'
                })
                
            except Exception as e:
                logger.error(f"❌ Fehler bei Task {task.get('id', 'N/A')}: {str(e)}", exc_info=True)
                predictions.append({'result': [], 'score': 0.0})
        
        logger.info(f"\n{'='*60}")
        logger.info(f"✅ Predictions abgeschlossen: {len(predictions)} Ergebnis(se)")
        logger.info(f"{'='*60}\n")
        
        return predictions

    def fit(self, annotations, **kwargs):
        return {
            'status': 'Training muss manuell durchgeführt werden',
            'info': 'Nutzen Sie train_model_bbox.py für das Training'
        }