from label_studio_ml.api import init_app
from yolo_backend import YOLODetectionModel  # Geändert von YOLOSegmentationModel!
import logging
import sys

# Ausführliches Logging
logging.basicConfig(
    level=logging.DEBUG,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s',
    handlers=[
        logging.StreamHandler(sys.stdout)
    ]
)

logger = logging.getLogger(__name__)

if __name__ == '__main__':
    print("=" * 70)
    print("🚀 Label Studio ML Backend - Detection (Bounding Boxes)")
    print("=" * 70)
    print()
    print("📍 Backend URL: http://localhost:9090")
    print("💡 In Label Studio unter Settings → Machine Learning hinzufügen")
    print("⌨️  Strg+C zum Beenden")
    print()
    print("📋 Warte auf Requests...")
    print("=" * 70)
    print()
    
    try:
        app = init_app(model_class=YOLODetectionModel)  # Geändert!
        
        # Flask Logging aktivieren
        app.config['DEBUG'] = True
        
        # CORS für lokale Entwicklung
        @app.after_request
        def after_request(response):
            logger.info(f"Request: {response.status}")
            return response
        
        app.run(host='localhost', port=9090, debug=True, use_reloader=False)
        
    except Exception as e:
        logger.error(f"❌ Fehler beim Starten: {e}", exc_info=True)
        sys.exit(1)