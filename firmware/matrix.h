LedControl TC_LED = LedControl(13, 26, 25, 7);  //DIN, CLK, CS, numDevices 13

LedControl DATA_LED = LedControl(12, 26, 25, 9);

int brightness = 8;


bool blinking = false;


unsigned char PROGMEM font_number[11][5] = {
  { 0x3E, 0x45, 0x49, 0x51, 0x3E },  // 0 - 9
  { 0x00, 0x21, 0x7F, 0x1, 0x0 },
  { 0x21, 0x43, 0x45, 0x49, 0x31 },
  { 0x22, 0x41, 0x49, 0x49, 0x36 },
  { 0xC, 0x14, 0x24, 0x7F, 0x4 },
  { 0x72, 0x51, 0x51, 0x51, 0x4E },
  { 0x3E, 0x49, 0x49, 0x49, 0x26 },
  { 0x40, 0x40, 0x4F, 0x50, 0x60 },
  { 0x36, 0x49, 0x49, 0x49, 0x36 },
  { 0x32, 0x49, 0x49, 0x49, 0x3E },
  { 0, 0, 0, 0, 0 },  // space
};

unsigned char PROGMEM font_letter[37][5] = {
  { 0x3E, 0x45, 0x49, 0x51, 0x3E },  // 0 - 9
  { 0x00, 0x21, 0x7F, 0x1, 0x0 },
  { 0x21, 0x43, 0x45, 0x49, 0x31 },
  { 0x22, 0x41, 0x49, 0x49, 0x36 },
  { 0xC, 0x14, 0x24, 0x7F, 0x4 },
  { 0x72, 0x51, 0x51, 0x51, 0x4E },
  { 0x3E, 0x49, 0x49, 0x49, 0x26 },
  { 0x40, 0x40, 0x4F, 0x50, 0x60 },
  { 0x36, 0x49, 0x49, 0x49, 0x36 },
  { 0x32, 0x49, 0x49, 0x49, 0x3E },
  { 0, 0, 0, 0, 0 },                 // space
  { 0x3f, 0x48, 0x48, 0x48, 0x3f },  // A
  { 0x7f, 0x49, 0x49, 0x49, 0x36 },
  { 0x3e, 0x41, 0x41, 0x41, 0x22 },
  { 0x7f, 0x41, 0x41, 0x22, 0x1c },
  { 0x7f, 0x49, 0x49, 0x49, 0x41 },
  { 0x7f, 0x48, 0x48, 0x48, 0x40 },
  { 0x3e, 0x41, 0x49, 0x49, 0x2e },
  { 0x7f, 0x08, 0x08, 0x08, 0x7f },
  { 0x00, 0x41, 0x7f, 0x41, 0x00 },
  { 0x06, 0x01, 0x01, 0x01, 0x7e },
  { 0x7f, 0x08, 0x14, 0x22, 0x41 },
  { 0x7f, 0x01, 0x01, 0x01, 0x01 },
  { 0x7f, 0x20, 0x10, 0x20, 0x7f },
  { 0x7f, 0x10, 0x08, 0x04, 0x7f },
  { 0x3e, 0x41, 0x41, 0x41, 0x3e },
  { 0x7f, 0x48, 0x48, 0x48, 0x30 },
  { 0x3e, 0x41, 0x45, 0x42, 0x3d },
  { 0x7f, 0x48, 0x4c, 0x4a, 0x31 },
  { 0x31, 0x49, 0x49, 0x49, 0x46 },
  { 0x40, 0x40, 0x7f, 0x40, 0x40 },
  { 0x7e, 0x01, 0x01, 0x01, 0x7e },
  { 0x7c, 0x02, 0x01, 0x02, 0x7c },
  { 0x7f, 0x02, 0x04, 0x02, 0x7f },
  { 0x63, 0x14, 0x08, 0x14, 0x63 },
  { 0x60, 0x10, 0x0f, 0x10, 0x60 },
  { 0x43, 0x45, 0x49, 0x51, 0x61 },  // Z
};

void rotateMatrixCW(unsigned char* matrix) {
  unsigned char temp[8] = { 0 };
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (matrix[i] & (1 << j)) {
        temp[7 - j] |= (1 << i);
      }
    }
  }
  for (int i = 0; i < 8; i++) {
    matrix[i] = temp[i];
  }
}

void fill_buffer(unsigned char* buffer, const char* text, int bufferWidth, bool shot = false) {
  // Puffer mit 0 initialisieren
  for (int i = 0; i < bufferWidth; i++) {
    buffer[i] = 0;
  }

  int colIndex = 0;     // Aktuelle Spalte im Puffer
  int letterIndex = 0;  // Aktueller Buchstabe im Eingabestring

  // Puffer füllen: pro Zeichen 5 Spalten aus dem Font plus 1 Spalte Abstand
  while (colIndex < bufferWidth) {
    if (text[letterIndex] != '\0') {
      char c = text[letterIndex];
      unsigned char letterData[5];

      // Zeichen in Font-Index umwandeln
      if (c >= 'A' && c <= 'Z') {
        int index = c - 'A' + 11;
        if (index >= 0 && index < 37) {
          memcpy_P(letterData, font_letter[index], 5);
        } else {
          memset(letterData, 0, 5);
        }
      } else if (c >= '0' && c <= '9') {
        int index = c - '0';
        if (index >= 0 && index < 37) {
          memcpy_P(letterData, font_letter[index], 5);
        } else {
          memset(letterData, 0, 5);
        }
      } else {
        memset(letterData, 0, 5);  // Unbekanntes Zeichen → leer
      }

      // Platz prüfen
      if (colIndex + 5 <= bufferWidth) {
        for (int i = 0; i < 5; i++) {
          buffer[colIndex++] = letterData[i];
        }

        // Standard-Abstand nach jedem Zeichen
        if (colIndex < bufferWidth) {
          buffer[colIndex++] = 0;
        }

        // ⬅️ Zusätzlicher Abstand NACH dem zweiten Zeichen (d. h. nach Index 1)
        if (shot && letterIndex == 1 && colIndex < bufferWidth) {
          buffer[colIndex++] = 0;
        }

      } else {
        // Kein Platz mehr für komplettes Zeichen → Rest mit 0 auffüllen
        while (colIndex < bufferWidth) {
          buffer[colIndex++] = 0;
        }
      }

      letterIndex++;  // Nächster Buchstabe
    } else {
      // String zu Ende → Rest mit 0 auffüllen
      while (colIndex < bufferWidth) {
        buffer[colIndex++] = 0;
      }
      break;
    }
  }
}

void display_text(const char* text) {

  for (int n = 0; n < 7; n++) {
    TC_LED.shutdown(n, false);
    TC_LED.setIntensity(n, 8);
  }

  unsigned char textBuffer[56];
  fill_buffer(textBuffer, text, 56);

  // 2D-Array für die Matrix
  unsigned char letMatrix[7][8] = { 0 };

  // Befüllen der Matrix mit Werten aus textBuffer in umgekehrter Reihenfolge
  for (int i = 0; i < 7; i++) {
    for (int j = 0; j < 8; j++) {
      letMatrix[i][j] = textBuffer[55 - (i * 8 + j)];
    }
  }

  // Rotation der einzelnen Zeilen mit rotateMatrixCW
  for (int i = 0; i < 7; i++) {
    rotateMatrixCW(letMatrix[i]);
  }

  // Setzen der Zeilen in TC_LED
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 7; j++) {
      TC_LED.setRow(6 - j, i, letMatrix[6 - j][i]);  // Reihenfolge umdrehen
    }
  }
}

void display_shot(const char* text) {
  // Erst alte Daten löschen
  for (int n = 3; n < 6; n++) {
    DATA_LED.clearDisplay(n);
    DATA_LED.shutdown(n, false);
    DATA_LED.setIntensity(n, brightness);
  }
  
  unsigned char shotBuffer[24];
  fill_buffer(shotBuffer, text, 24);

  unsigned char shotMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      shotMatrix[i][j] = shotBuffer[23 - (i * 8 + j)];
    }
  }

  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(shotMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 3; j < 6; j++) {
      DATA_LED.setRow(j, i, shotMatrix[j - 3][i]);
    }
  }
}

void display_roll(const char* text) {
  // Erst alte Daten löschen
  for (int n = 0; n < 3; n++) {
    DATA_LED.clearDisplay(n);
    DATA_LED.shutdown(n, false);
    DATA_LED.setIntensity(n, brightness);
  }
  
  unsigned char rollBuffer[24];
  fill_buffer(rollBuffer, text, 24);

  unsigned char rollMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      rollMatrix[i][j] = rollBuffer[23 - (i * 8 + j)];
    }
  }

  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(rollMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 3; j++) {
      DATA_LED.setRow(j, i, rollMatrix[j][i]);
    }
  }
}

void display_scene(int scene, int take) {
  char textStr[5];
  sprintf(textStr, "%2d%2d", scene, take);

  display_text(textStr);

  // Erst alte Daten löschen
  for (int n = 6; n < 9; n++) {
    DATA_LED.clearDisplay(n);
    DATA_LED.shutdown(n, false);
    DATA_LED.setIntensity(n, brightness);
  }
  
  unsigned char sceneBuffer[24];
  fill_buffer(sceneBuffer, textStr, 24, true);

  unsigned char sceneMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      sceneMatrix[i][j] = sceneBuffer[23 - (i * 8 + j)];
    }
  }

  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(sceneMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 6; j < 9; j++) {
      DATA_LED.setRow(j, i, sceneMatrix[j - 6][i]);
    }
  }
}
void display_TC(int hour, int minute, int second, int frame) {

  for (int n = 0; n < 7; n++) {
    TC_LED.shutdown(n, false);
    TC_LED.setIntensity(n, brightness);
  }

  int hour_digit_0 = hour % 10;
  int hour_digit_10 = (hour / 10) % 10;

  int minute_digit_0 = minute % 10;
  int minute_digit_10 = (minute / 10) % 10;

  int second_digit_0 = second % 10;
  int second_digit_10 = (second / 10) % 10;

  int frame_digit_0 = frame % 10;
  int frame_digit_10 = ((frame / 10) % 10);

  unsigned char numMatrix_0[8] = { 0 };
  unsigned char numMatrix_1[8] = { 0 };
  unsigned char numMatrix_2[8] = { 0 };
  unsigned char numMatrix_3[8] = { 0 };
  unsigned char numMatrix_4[8] = { 0 };
  unsigned char numMatrix_5[8] = { 0 };
  unsigned char numMatrix_6[8] = { 0 };

  // Laden der Font-Daten
  numMatrix_0[0] = font_number[10][0];
  numMatrix_0[1] = font_number[frame_digit_0][4];
  numMatrix_0[2] = font_number[frame_digit_0][3];
  numMatrix_0[3] = font_number[frame_digit_0][2];
  numMatrix_0[4] = font_number[frame_digit_0][1];
  numMatrix_0[5] = font_number[frame_digit_0][0];
  numMatrix_0[6] = font_number[10][0];
  numMatrix_0[7] = font_number[frame_digit_10][4];
  numMatrix_1[0] = font_number[frame_digit_10][3];
  numMatrix_1[1] = font_number[frame_digit_10][2];
  numMatrix_1[2] = font_number[frame_digit_10][1];
  numMatrix_1[3] = font_number[frame_digit_10][0];
  numMatrix_1[4] = font_number[10][0];
  numMatrix_1[5] = font_number[4][1];
  numMatrix_1[6] = font_number[10][0];
  numMatrix_1[7] = font_number[second_digit_0][4];
  numMatrix_2[0] = font_number[second_digit_0][3];
  numMatrix_2[1] = font_number[second_digit_0][2];
  numMatrix_2[2] = font_number[second_digit_0][1];
  numMatrix_2[3] = font_number[second_digit_0][0];
  numMatrix_2[4] = font_number[10][0];
  numMatrix_2[5] = font_number[second_digit_10][4];
  numMatrix_2[6] = font_number[second_digit_10][3];
  numMatrix_2[7] = font_number[second_digit_10][2];
  numMatrix_3[0] = font_number[second_digit_10][1];
  numMatrix_3[1] = font_number[second_digit_10][0];
  numMatrix_3[2] = font_number[10][0];
  numMatrix_3[3] = font_number[4][1];
  numMatrix_3[4] = font_number[10][0];
  numMatrix_3[5] = font_number[minute_digit_0][4];
  numMatrix_3[6] = font_number[minute_digit_0][3];
  numMatrix_3[7] = font_number[minute_digit_0][2];
  numMatrix_4[0] = font_number[minute_digit_0][1];
  numMatrix_4[1] = font_number[minute_digit_0][0];
  numMatrix_4[2] = font_number[10][0];
  numMatrix_4[3] = font_number[minute_digit_10][4];
  numMatrix_4[4] = font_number[minute_digit_10][3];
  numMatrix_4[5] = font_number[minute_digit_10][2];
  numMatrix_4[6] = font_number[minute_digit_10][1];
  numMatrix_4[7] = font_number[minute_digit_10][0];
  numMatrix_5[0] = font_number[10][0];
  numMatrix_5[1] = font_number[4][1];
  numMatrix_5[2] = font_number[10][0];
  numMatrix_5[3] = font_number[hour_digit_0][4];
  numMatrix_5[4] = font_number[hour_digit_0][3];
  numMatrix_5[5] = font_number[hour_digit_0][2];
  numMatrix_5[6] = font_number[hour_digit_0][1];
  numMatrix_5[7] = font_number[hour_digit_0][0];
  numMatrix_6[0] = font_number[10][0];
  numMatrix_6[1] = font_number[hour_digit_10][4];
  numMatrix_6[2] = font_number[hour_digit_10][3];
  numMatrix_6[3] = font_number[hour_digit_10][2];
  numMatrix_6[4] = font_number[hour_digit_10][1];
  numMatrix_6[5] = font_number[hour_digit_10][0];
  numMatrix_6[6] = font_number[10][0];
  numMatrix_6[7] = font_number[10][0];

  // Rotieren der Matrizen im Uhrzeigersinn
  rotateMatrixCW(numMatrix_0);
  rotateMatrixCW(numMatrix_1);
  rotateMatrixCW(numMatrix_2);
  rotateMatrixCW(numMatrix_3);
  rotateMatrixCW(numMatrix_4);
  rotateMatrixCW(numMatrix_5);
  rotateMatrixCW(numMatrix_6);

  // Daten an die LED-Matrizen senden
  for (int i = 0; i < 8; i++) {
    TC_LED.setRow(0, i, numMatrix_0[i]);
    TC_LED.setRow(1, i, numMatrix_1[i]);
    TC_LED.setRow(2, i, numMatrix_2[i]);
    TC_LED.setRow(3, i, numMatrix_3[i]);
    TC_LED.setRow(4, i, numMatrix_4[i]);
    TC_LED.setRow(5, i, numMatrix_5[i]);
    TC_LED.setRow(6, i, numMatrix_6[i]);
  }
}

/*

void display_data(const char* roll, const char* shot, int scene, int take) {
  
  // Alle 9 Devices auf einmal initialisieren
  for (int n = 0; n < 9; n++) {
    DATA_LED.clearDisplay(n);
    DATA_LED.shutdown(n, false);
    DATA_LED.setIntensity(n, brightness);
  }

  // ========== ROLL (Devices 0-2) ==========
  unsigned char rollBuffer[24];
  fill_buffer(rollBuffer, roll, 24);

  unsigned char rollMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      rollMatrix[i][j] = rollBuffer[23 - (i * 8 + j)];
    }
  }

  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(rollMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 3; j++) {
      DATA_LED.setRow(j, i, rollMatrix[j][i]);
    }
  }

  // ========== SHOT (Devices 3-5) - GEGEN Uhrzeigersinn ==========
  unsigned char shotBuffer[24];
  fill_buffer(shotBuffer, shot, 24);

  unsigned char shotMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      shotMatrix[i][j] = shotBuffer[23 - (i * 8 + j)];
    }
  }

  // ✅ GEGEN Uhrzeigersinn = 3x im Uhrzeigersinn rotieren
  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(shotMatrix[i]);
    rotateMatrixCW(shotMatrix[i]);
    rotateMatrixCW(shotMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 3; j++) {
      DATA_LED.setRow(5 - j, i, shotMatrix[j][i]);
    }
  }

  // ========== SCENE (Devices 6-8) ==========
  char sceneStr[5];
  sprintf(sceneStr, "%2d%2d", scene, take);

  unsigned char sceneBuffer[24];
  fill_buffer(sceneBuffer, sceneStr, 24, true);

  unsigned char sceneMatrix[3][8] = { 0 };

  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 8; j++) {
      sceneMatrix[i][j] = sceneBuffer[23 - (i * 8 + j)];
    }
  }

  for (int i = 0; i < 3; i++) {
    rotateMatrixCW(sceneMatrix[i]);
  }

  for (int i = 0; i < 8; i++) {
    for (int j = 6; j < 9; j++) {
      DATA_LED.setRow(j, i, sceneMatrix[j - 6][i]);
    }
  }
}
*/