#include <Wire.h>
#include <RTClib.h>
#include <esp_now.h>
#include <WiFi.h>
#include <WifiMulti.h>
#include <LedControl.h>
#include <esp_timer.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <ezButton.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>

#include "arc.h"
#include "timecode.h"
#include "matrix.h"
#include "e-ink-display.h"

// --- Globale Variablen und Objekte ---
constexpr int clapButton = 32;

WiFiMulti wifiMulti;

constexpr int buttonPin1 = 34;
constexpr int buttonPin2 = 39;
constexpr int buttonPin3 = 36;

bool lastState1 = false;
bool lastState2 = false;
bool lastState3 = false;
int lastClapState = LOW;



String lastShot = "NONE";
int lastTake = 0;
String lastRoll = "NONE";

bool showTC = true;

int currentMode = 0;
constexpr int NUM_MODES = 5;
const char *modeNames[NUM_MODES] = {
  "Idle",
  "SET TAKE",
  "SET SHOT",
  "SET ROLL",
  "SET CAM"
};


char TCText[32] = "EMPTY";

constexpr int lightPin = 17;
unsigned long lightOffAt = 0;

int rollNum = 1;
char cam = 'A';
char roll[6] = "A000";  // 5 + null
char shot[16] = "NONE";
int scene = 0;
int take = 0;

String date = "00.00.0000";
String code = "-";
String production = "-";
String director = "-";
String camera = "-";
String environment = "-";
String daynight = "-";
int framerate = 24;
bool mos = false;

String ipAdress = "0";
String ssid = "0";

String arcReturn = "empty!";
int clappedHour = 0;
int clappedMinute = 0;
int clappedSecond = 0;
int clappedFrame = 0;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncWebSocketClient *lastClient = nullptr;

bool display_text_change = false;
bool display_roll_change = false;
bool display_scene_change = false;
bool display_shot_change = false;
bool display_show_clapped_TC = false;


// --- Hilfsfunktionen ---
void notifyClients(const String &message) {
  // ws.textAll(message); // ggf. für Broadcast
}

void sendJsonResponse(AsyncWebSocketClient *client, const char *type) {
  StaticJsonDocument<256> response;
  if (strcmp(type, "newTake") == 0) {
    char tcBuffer[12];
    snprintf(tcBuffer, sizeof(tcBuffer), "%02d:%02d:%02d:%02d", clappedHour, clappedMinute, clappedSecond, clappedFrame);
    response["type"] = type;
    response["roll"] = roll;
    response["take"] = take;
    response["timecode"] = tcBuffer;
    response["date"] = date;
  } else if (strcmp(type, "nextShot") == 0 || strcmp(type, "previousShot") == 0 || strcmp(type, "activateShot") == 0) {
    response["type"] = type;
  }
  String jsonResponse;
  serializeJson(response, jsonResponse);
  if (client) client->text(jsonResponse);
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DATA) {
    lastClient = client;
    showTC = false;
    if (currentMode == 0) {
      strncpy(TCText, "SYNC ARC", sizeof(TCText));
      display_text_change = true;
    }


    String jsonStr;
    for (size_t i = 0; i < len; i++) jsonStr += (char)data[i];

    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, jsonStr);
    if (!error) {
      scene = doc["scene"].as<int>();
      strncpy(shot, doc["shot"] | "NONE", sizeof(shot));
      take = doc["take"].as<int>();
      production = doc["production"] | "-";
      director = doc["director"] | "-";
      camera = doc["camera"] | "-";
      code = doc["code"] | "-";
      framerate = doc["framerate"].as<int>();
      mos = doc["mos"].as<bool>();
      environment = doc["environment"] | "-";
      daynight = doc["daynight"] | "-";

      client->text("Empfangen");

      display_roll_change = true;
      display_shot_change = true;
      display_scene_change = true;

      DebugLog("Received Data: " + String(scene) + ", " + shot + ", " + String(take) + ", " + code + ", " + production + ", " + director + ", " + camera + ", " + date + ", " + framerate + ", " + mos + ", " + daynight + ", " + environment);
      // Startet eine eigene Task für das zeitintensive E-Paper Update
      if (currentMode == 0) {
        xTaskCreate([](void *) {
          vTaskDelay(pdMS_TO_TICKS(50));  // jetzt erlaubt!
          drawEPaper(roll, scene, shot, take, code, production, director, camera, date, framerate, mos, daynight, environment);
          lastShot = shot;
          lastTake = take;
          lastRoll = roll;
          showTC = true;
          vTaskDelete(NULL);  // Task beenden
        },
                    "EPaperTask", 8192, NULL, 1, NULL);
      } else {
        strncpy(TCText, modeNames[currentMode], sizeof(TCText));
        display_text_change = true;
      }

    } else {
      client->text("Fehler: Ungültiges JSON");
    }
  }
}


void arcTask(void *parameter) {
  showTC = false;
  display.init();
  display.setRotation(0);
  display.setTextColor(GxEPD_BLACK);


  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  server.begin();

  do {
    display.drawBitmap(0, 0, bootscreen_V2, 800, 480, GxEPD_WHITE);
    display.setFont(&FreeSansBold24pt7b);
    display.setCursor(160, 370);
    display.print("NO PROJECT LOADED");

    display.setFont(&FreeSansOblique18pt7b);
    display.setCursor(0, 475);
    display.print("IP-Adress: " + ipAdress);
    display.setCursor(0, 430);
    display.print("Connected to " + ssid);


  } while (display.nextPage());


  showTC = true;
  strncpy(TCText, modeNames[0], sizeof(TCText));

  while (true) {  //Nur, wenn es ein Projekt gibt
    if (code != "-") {


      vTaskDelay(pdMS_TO_TICKS(50));
      if (currentMode == 0) {
        int currentClapState = digitalRead(clapButton);

        // --- Clap gedrückt ---
        if (currentClapState == LOW && lastClapState == HIGH) {
          digitalWrite(lightPin, HIGH);
          clappedFrame = frameCount;
          clappedSecond = secondCount;
          clappedMinute = minuteCount;
          clappedHour = hourCount;

          if (lastClient) sendJsonResponse(lastClient, "newTake");
          showTC = false;
          display_show_clapped_TC = true;
          DebugLog("Clap closed, Light on");
          lightOffAt = millis() + 1000 / framerate;
        }

        // --- Licht aus nach Zeit ---
        if (lightOffAt > 0 && millis() >= lightOffAt) {
          digitalWrite(lightPin, LOW);
          lightOffAt = 0;
          DebugLog("Light off");
        }


        // --- Clap losgelassen ---
        if (currentClapState == HIGH && lastClapState == LOW) {
          currentMode = 1;
          showTC = false;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_text_change = true;
          display_scene_change = true;
          take++;
        }

        lastClapState = currentClapState;
      }


      // --- Button 1 ---
      bool currentState1 = digitalRead(buttonPin1);
      if (currentState1 && !lastState1) {
        showTC = false;
        if (currentMode == 1 && take > 0) {
          take--;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_scene_change = true;
          display_text_change = true;
        } else if (currentMode == 2) {
          DebugLog("Previous Shot");
          sendJsonResponse(lastClient, "previousShot");
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_shot_change = true;
          display_text_change = true;
        } else if (currentMode == 3 && rollNum > 0) {
          rollNum--;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_roll_change = true;
          display_text_change = true;
        } else if (currentMode == 4 && cam > 'A') {
          cam--;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_roll_change = true;
          display_text_change = true;
        }
        snprintf(roll, sizeof(roll), "%c%03d", cam, rollNum);
      }
      lastState1 = currentState1;

      // --- Button 2 (Moduswechsel) ---
      bool currentState2 = digitalRead(buttonPin2);
      if (currentState2 && !lastState2) {
        DebugLog("Taste 2 gedrückt – Funktion wird ausgeführt");
        currentMode = (currentMode + 1) % NUM_MODES;
        strncpy(TCText, modeNames[currentMode], sizeof(TCText));
        display_text_change = true;
        showTC = (currentMode == 0);
        // Nach Moduswechsel immer aktuelle Werte anzeigen
        display_scene_change = true;
        display_shot_change = true;
        display_roll_change = true;
        if (currentMode == 0 && (lastShot != shot || lastTake != take || lastRoll != roll)) {
          drawEPaper(roll, scene, shot, take, code, production, director, camera, date, framerate, mos, daynight, environment);

          // Werte aktualisieren
          lastShot = shot;
          lastTake = take;
          lastRoll = roll;
        }
      }
      lastState2 = currentState2;




      // --- Button 3 ---
      bool currentState3 = digitalRead(buttonPin3);
      if (currentState3 && !lastState3) {
        if (currentMode == 1 && take < 99) {
          take++;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_scene_change = true;
          display_text_change = true;
        } else if (currentMode == 2) {
          DebugLog("Next Shot");
          sendJsonResponse(lastClient, "nextShot");
        } else if (currentMode == 3 && rollNum < 999) {
          rollNum++;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_roll_change = true;
          display_text_change = true;
        } else if (currentMode == 4 && cam < 'Z') {
          cam++;
          strncpy(TCText, modeNames[currentMode], sizeof(TCText));
          display_roll_change = true;
          display_text_change = true;
        }
        snprintf(roll, sizeof(roll), "%c%03d", cam, rollNum);
      }
      lastState3 = currentState3;
    }
  }
}

void LEDTask(void *parameter) {
  while (true) {
    if (showTC) {
      // Timecode um 1 Frame zurückrechnen
      int h = hourCount;
      int m = minuteCount;
      int s = secondCount;
      int f = frameCount;

      // Rückwärts zählen
      f--;
      if (f < 0) {
        f = framerate - 1;
        s--;
        if (s < 0) {
          s = 59;
          m--;
          if (m < 0) {
            m = 59;
            h--;
            if (h < 0) {
              h = 23;  // wrap-around bei SMPTE-Style
            }
          }
        }
      }

      // Ausgabe des "1 Frame früheren" Timecodes
      display_TC(h, m, s, f);

      // Delay wie gehabt
      vTaskDelay(pdMS_TO_TICKS(1000 / (framerate * 2)));
    } else {
      if (display_roll_change) {
        display_roll(roll);
        display_roll_change = false;
      }
      if (display_shot_change) {
        display_shot(shot);
        display_shot_change = false;
      }
      if (display_scene_change) {
        display_scene(scene, take);
        display_scene_change = false;
      }
      if (display_text_change) {
        display_text(TCText);
        display_text_change = false;
      }
      if (display_show_clapped_TC) {
        display_TC(clappedHour, clappedMinute, clappedSecond, clappedFrame);
        display_show_clapped_TC = false;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

void TCTask(void *parameter) {
  Wire.begin();

  // NTP-Client starten
  timeClient.begin();
  timeClient.update();

  // ESP-NOW Setup
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    DebugLog("ESP-NOW Init failed!");
    return;
  }

  // RTC Setup
  if (!rtc.begin()) {
    DebugLog("Couldn't find RTC");
    showTC = false;
    strncpy(TCText, "LOST RTC", sizeof(TCText));
    while (1)
      ;
  }
  if (rtc.lostPower()) {
    DebugLog("RTC is NOT running!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    DebugLog("RTC adjusted!");
  }

  if (syncTime) {
    strncpy(TCText, "SYNC RTC", sizeof(TCText));
    display_text_change = true;

    // Setze Zeitzone: Mitteleuropäische Zeit inkl. Sommerzeitregelung
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");

    timeClient.update();
    DebugLog("Synchronisiere Zeit mit dem NTP-Server...");

    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      rtc.adjust(DateTime(
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec));
      DebugLog("RTC aktualisiert.");
    } else {
      DebugLog("getLocalTime() fehlgeschlagen.");
    }

    DebugLog("Aktuelle Zeit vom NTP-Server: ");
    DebugLog(timeClient.getFormattedTime());  // Achtung: Gibt immer UTC zurück!
  }


  // Initialisiere Timecode-Variablen basierend auf der RTC
  DateTime now = rtc.now();
  hourCount = now.hour();
  minuteCount = now.minute();
  secondCount = now.second();
  char buf[11];
  sprintf(buf, "%02d.%02d.%04d", now.day(), now.month(), now.year());
  date = String(buf);
  DebugLog(date);
  data.hourCountSend = hourCount;
  data.minuteCountSend = minuteCount;
  data.secondCountSend = secondCount;
  data.frameCountSend = frameCount;
  DebugLog(String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second()));


  // Output-Pin initialisieren
  pinMode(tlcpin, OUTPUT);
  // PWM Setup (z. B. 50% PWM Ground Level)
  ledcSetup(0, 1000000 / framerate, 8);
  ledcAttachPin(tlcpin, 0);
  ledcWrite(0, groundLevel);

  lastRtcSyncTime = rtc.now();
  lastMicros = micros();

  while (true) {

    // Aktualisierung des internen Timers
    unsigned long long currentMicros = getAdjustedMicros();
    if (currentMicros - previousMicros >= 1000000) {
      previousMicros = currentMicros;
      // Korrigiere ggf. die Frequenz
      ledcChangeFrequency(0, 1000000 / framerate + 1 / framerate, 8);
      secondCounter = 0;
    }

    // ESP-NOW Send/Receive Logik (synchronisieren, falls nötig)
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= tempinterval) {
      previousMillis = currentMillis;
    }

    // TC-Frame-Timer
    if (getAdjustedMicros() >= newTimer) {
      newTimer += 1000000 / framerate / 80 / 2;
      if (secondCounter < framerate * 80 * 2) {
        timerInterrupt();
      }
      secondCounter++;
    }
  }
}

void setup() {
  beginDebug();
  pinMode(lightPin, OUTPUT);
  pinMode(buttonPin1, INPUT);
  pinMode(buttonPin2, INPUT);
  pinMode(buttonPin3, INPUT);
  pinMode(clapButton, INPUT);

  // Bekannte WLANs hinzufügen
  //wifiMulti.addAP("Vodafone-F34C", "cgxY9rLxLzgc2Xhc");
  wifiMulti.addAP("JULIAN-IPHONE", "012345678909");
  wifiMulti.addAP("MagentaWLAN-R38Y", "88255808344892594692");
  wifiMulti.addAP("Probst", "9578674513537646");
  wifiMulti.addAP("ARC-ONE", "12345678");
  wifiMulti.addAP("24p Film Studio", "76915661820278777668");
  wifiMulti.addAP("Berserker", "76915661820278777668");
  DebugLog("Verbindung wird hergestellt...");
  display_text("CONNECT");

  const int maxRetries = 2;
  int retryCount = 0;
  while (wifiMulti.run() != WL_CONNECTED && retryCount < maxRetries) {
    delay(1000);
    retryCount++;
    DebugLog("Warte auf WLAN...");
  }
  if (WiFi.isConnected()) {
    DebugLog("WLAN verbunden!");
    display_text("CONNECTED");
    ipAdress = WiFi.localIP().toString();
    ssid = WiFi.SSID();
    if (ssid == "ARC-ONE") {
      syncTime = false;
    } else {
      syncTime = true;
    }
  } else {
    DebugLog("WLAN-Verbindung fehlgeschlagen!");
    display_text("NO WIFI");
    syncTime = false;
    ipAdress = "None";
    ssid = "no Wifi";
    delay(2000);
  }

  snprintf(roll, sizeof(roll), "%c%03d", cam, rollNum);
  //display_data(roll, shot, scene, take);
  display_roll(roll);
  display_shot(shot);
  display_scene(scene, take);


  //----------------------------------------------Threating----------------------------------------------//
  xTaskCreatePinnedToCore(
    LEDTask,    // Task-Funktion
    "LEDTask",  // Name der Task
    12288,      // Stack-Größe (erhöht!)
    NULL,       // Parameter
    3,          // Priorität (anpassen, falls nötig)
    NULL,       // Task-Handle (optional)
    0           // Core-ID (auf dem ESP32 gibt es zwei Kerne: 0 und 1)
  );

  xTaskCreatePinnedToCore(
    arcTask,    // Task-Funktion
    "arcTask",  // Name der Task
    12288,      // Stack-Größe (erhöht!)
    NULL,       // Parameter
    0,          // Priorität (anpassen, falls nötig)
    NULL,       // Task-Handle (optional)
    0           // Core-ID (auf dem ESP32 gibt es zwei Kerne: 0 und 1)
  );

  xTaskCreatePinnedToCore(
    TCTask,    // Task-Funktion
    "TCTask",  // Name der Task
    12288,     // Stack-Größe (erhöht!)
    NULL,      // Parameter
    2,         // Priorität (anpassen, falls nötig)
    NULL,      // Task-Handle (optional)
    1          // Core 1 immer für die TC Ausgabe
  );
}

void loop() {
}
