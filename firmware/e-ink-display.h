#include "data/FreeSansOblique18pt7b.h"
#include "data/FreeSansBold9pt7b.h"
#include "data/FreeSansBold24pt7b.h"
#include "data/FreeSansBold18pt7b.h"
#include "data/bootscreen_V2.h"
#include "data/logo.h"

#include <GxEPD2_BW.h>        // GxEPD2-Library für E-Ink-Displays
#include "QRCodeGenerator.h"  // Felix Erdmanns QR-Code-Bibliothek


GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(GxEPD2_750_T7(/*CS=5*/ 5, /*DC=*/33, /*RST=*/15, /*BUSY=*/16));  // GDEW075T7 800x480, EK79655 (GD7965)


void drawEPaper(String roll, int scene, String shot, int take, String code, String production, String director, String camera, String date, int framerate, bool mos, String daynight, String environment) {
  QRCode qrcode;

  String qrString;

  qrString = code + "_" + String(roll) + "_" + String(scene) + "_" + String(shot) + "_" + String(take);


  const char* qrData = qrString.c_str();

  uint8_t qrcodeData[qrcode_getBufferSize(3)];         // Verwende Version 3 für den QR-Code
  qrcode_initText(&qrcode, qrcodeData, 3, 0, qrData);  // Hier wird der Text für den QR-Code gesetzt

  //DateTime now = rtc.now();

  //sprintf(date, "%02d.%02d.%04d", now.day(), now.month(), now.year());

  display.firstPage();

  do {

    if (take != 0) {
      for (uint8_t y = 0; y < qrcode.size; y++) {
        for (uint8_t x = 0; x < qrcode.size; x++) {
          if (qrcode_getModule(&qrcode, x, y)) {
            // Zeichne ein einzelnes Modul als Quadrat
            display.fillRect(335 + x * 16, y * 16, 16, 16, GxEPD_BLACK);  // 4x4 Pixel pro Modul
          }
        }
      }
    } else {
      display.drawBitmap(400, 0, logo, 400, 130, GxEPD_WHITE);
      display.setFont(&FreeSansBold18pt7b);
      display.setCursor(390, 250);
      display.print("NO TAKE SELECTED");
    }
    display.fillRect(0, 90, 320, 3, GxEPD_BLACK);
    display.fillRect(0, 180, 320, 3, GxEPD_BLACK);
    display.fillRect(0, 270, 320, 3, GxEPD_BLACK);
    display.fillRect(0, 360, 320, 3, GxEPD_BLACK);

    display.fillRect(320, 0, 3, 480, GxEPD_BLACK);

    display.setFont(&FreeSansBold9pt7b);

    display.setCursor(0, 20);
    display.print("PRODUCTION:");
    display.setCursor(0, 110);
    display.print("DIRECTOR:");
    display.setCursor(0, 200);
    display.print("CAMERA:");
    display.setCursor(0, 290);
    display.print("DATE:");

    display.setFont(&FreeSansOblique18pt7b);

    display.setCursor(10, 70);
    display.print(production);
    display.setCursor(10, 160);
    display.print(director);
    display.setCursor(10, 250);
    display.print(camera);
    display.setCursor(10, 340);
    display.print(date);

    display.setFont(&FreeSansBold24pt7b);

    if (scene != 0) {

      display.setCursor(0, 425);
      display.print(environment);

      display.setCursor(0, 475);
      display.print(daynight);
    }


    if (take != 0) {
      display.setCursor(160, 425);
      String framerateString;
      framerateString = String(framerate) + "FPS";
      display.print(framerateString);

      display.setCursor(160, 475);
      if (mos) {
        display.print("MOS");
      }
    }
  } while (display.nextPage());
}