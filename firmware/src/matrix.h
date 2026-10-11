// matrix.h – MAX7219 8×8 LED matrix driver for ESP32 (no LedControl dependency)
// 7 modules daisy-chained: DIN=GPIO38, CS=GPIO39, CLK=GPIO40

#pragma once
#include <Arduino.h>

// ── Pin definitions ──────────────────────────────────────────────────
#define MATRIX_DIN  38
#define MATRIX_CS   39
#define MATRIX_CLK  40

// ── Global brightness (0–15) ─────────────────────────────────────────
static int brightness = 8;

// ── MAX7219 registers ────────────────────────────────────────────────
#define MAX7219_NOP         0x00
#define MAX7219_DECODE      0x09
#define MAX7219_INTENSITY   0x0A
#define MAX7219_SCAN_LIMIT  0x0B
#define MAX7219_SHUTDOWN    0x0C
#define MAX7219_DISPLAY_TEST 0x0F

// ── Low-level SPI helpers (software bit-banged) ──────────────────────
static inline void max7219_write_byte(uint8_t b) {
    for (int i = 0; i < 8; i++) {
        digitalWrite(MATRIX_CLK, LOW);
        digitalWrite(MATRIX_DIN, (b & 0x80) ? HIGH : LOW);
        b <<= 1;
        digitalWrite(MATRIX_CLK, HIGH);
    }
}

// Send a 16-bit command to ONE module in the daisy-chain.
static inline void max7219_send(int device, uint8_t opcode, uint8_t data) {
    digitalWrite(MATRIX_CS, LOW);
    for (int i = 6; i >= 0; i--) {
        if (i == device) {
            max7219_write_byte(opcode);
            max7219_write_byte(data);
        } else {
            max7219_write_byte(MAX7219_NOP);
            max7219_write_byte(0x00);
        }
    }
    digitalWrite(MATRIX_CS, HIGH);
}

// Broadcast same command to ALL 7 devices at once
static inline void max7219_broadcast(uint8_t opcode, uint8_t data) {
    digitalWrite(MATRIX_CS, LOW);
    for (int i = 0; i < 7; i++) {
        max7219_write_byte(opcode);
        max7219_write_byte(data);
    }
    digitalWrite(MATRIX_CS, HIGH);
}

// ── High-level helpers ───────────────────────────────────────────────
static inline void matrix_shutdown(int device, bool state) {
    max7219_send(device, MAX7219_SHUTDOWN, state ? 0x00 : 0x01);
}

static inline void matrix_setIntensity(int device, int intensity) {
    if (intensity < 0) intensity = 0;
    if (intensity > 15) intensity = 15;
    max7219_send(device, MAX7219_INTENSITY, (uint8_t)intensity);
}

static inline void matrix_clearDisplay(int device) {
    for (int row = 0; row < 8; row++) {
        max7219_send(device, row + 1, 0x00);
    }
}

static inline void matrix_setRow(int device, int row, uint8_t value) {
    if (row < 0 || row > 7) return;
    max7219_send(device, row + 1, value);
}

// Initialize all 7 modules
static inline void matrix_init_all() {
    pinMode(MATRIX_DIN, OUTPUT);
    pinMode(MATRIX_CLK, OUTPUT);
    pinMode(MATRIX_CS,  OUTPUT);
    digitalWrite(MATRIX_DIN, LOW);
    digitalWrite(MATRIX_CLK, LOW);
    digitalWrite(MATRIX_CS,  HIGH);

    for (int n = 0; n < 7; n++) {
        max7219_send(n, MAX7219_SHUTDOWN, 0x01);
        max7219_send(n, MAX7219_DECODE, 0x00);
        max7219_send(n, MAX7219_SCAN_LIMIT, 0x07);
        max7219_send(n, MAX7219_DISPLAY_TEST, 0x00);
        matrix_setIntensity(n, brightness);
        matrix_clearDisplay(n);
    }
}

// ── Font data ────────────────────────────────────────────────────────
static const unsigned char font_number[11][5] = {
  { 0x3E, 0x45, 0x49, 0x51, 0x3E },
  { 0x00, 0x21, 0x7F, 0x01, 0x00 },
  { 0x21, 0x43, 0x45, 0x49, 0x31 },
  { 0x22, 0x41, 0x49, 0x49, 0x36 },
  { 0x0C, 0x14, 0x24, 0x7F, 0x04 },
  { 0x72, 0x51, 0x51, 0x51, 0x4E },
  { 0x3E, 0x49, 0x49, 0x49, 0x26 },
  { 0x40, 0x40, 0x4F, 0x50, 0x60 },
  { 0x36, 0x49, 0x49, 0x49, 0x36 },
  { 0x32, 0x49, 0x49, 0x49, 0x3E },
  { 0x00, 0x00, 0x00, 0x00, 0x00 },  // space (10)
};

static const unsigned char font_letter[37][5] = {
  { 0x3E, 0x45, 0x49, 0x51, 0x3E },  // 0
  { 0x00, 0x21, 0x7F, 0x01, 0x00 },  // 1
  { 0x21, 0x43, 0x45, 0x49, 0x31 },  // 2
  { 0x22, 0x41, 0x49, 0x49, 0x36 },  // 3
  { 0x0C, 0x14, 0x24, 0x7F, 0x04 },  // 4
  { 0x72, 0x51, 0x51, 0x51, 0x4E },  // 5
  { 0x3E, 0x49, 0x49, 0x49, 0x26 },  // 6
  { 0x40, 0x40, 0x4F, 0x50, 0x60 },  // 7
  { 0x36, 0x49, 0x49, 0x49, 0x36 },  // 8
  { 0x32, 0x49, 0x49, 0x49, 0x3E },  // 9
  { 0x00, 0x00, 0x00, 0x00, 0x00 },  // space (10)
  { 0x3F, 0x48, 0x48, 0x48, 0x3F },  // A
  { 0x7F, 0x49, 0x49, 0x49, 0x36 },  // B
  { 0x3E, 0x41, 0x41, 0x41, 0x22 },  // C
  { 0x7F, 0x41, 0x41, 0x22, 0x1C },  // D
  { 0x7F, 0x49, 0x49, 0x49, 0x41 },  // E
  { 0x7F, 0x48, 0x48, 0x48, 0x40 },  // F
  { 0x3E, 0x41, 0x49, 0x49, 0x2E },  // G
  { 0x7F, 0x08, 0x08, 0x08, 0x7F },  // H
  { 0x00, 0x41, 0x7F, 0x41, 0x00 },  // I
  { 0x06, 0x01, 0x01, 0x01, 0x7E },  // J
  { 0x7F, 0x08, 0x14, 0x22, 0x41 },  // K
  { 0x7F, 0x01, 0x01, 0x01, 0x01 },  // L
  { 0x7F, 0x20, 0x10, 0x20, 0x7F },  // M
  { 0x7F, 0x10, 0x08, 0x04, 0x7F },  // N
  { 0x3E, 0x41, 0x41, 0x41, 0x3E },  // O
  { 0x7F, 0x48, 0x48, 0x48, 0x30 },  // P
  { 0x3E, 0x41, 0x45, 0x42, 0x3D },  // Q
  { 0x7F, 0x48, 0x4C, 0x4A, 0x31 },  // R
  { 0x31, 0x49, 0x49, 0x49, 0x46 },  // S
  { 0x40, 0x40, 0x7F, 0x40, 0x40 },  // T
  { 0x7E, 0x01, 0x01, 0x01, 0x7E },  // U
  { 0x7C, 0x02, 0x01, 0x02, 0x7C },  // V
  { 0x7F, 0x02, 0x04, 0x02, 0x7F },  // W
  { 0x63, 0x14, 0x08, 0x14, 0x63 },  // X
  { 0x60, 0x10, 0x0F, 0x10, 0x60 },  // Y
  { 0x43, 0x45, 0x49, 0x51, 0x61 },  // Z
};

// ── Helper: rotate 8-byte matrix 90° CW ──────────────────────────────
static void rotateMatrixCW(unsigned char* matrix) {
  unsigned char temp[8] = { 0 };
  for (int i = 0; i < 8; i++) {
    for (int j = 0; j < 8; j++) {
      if (matrix[i] & (1 << j)) {
        temp[7 - j] |= (1 << i);
      }
    }
  }
  for (int i = 0; i < 8; i++) matrix[i] = temp[i];
}

// ── Display timecode on 7× MAX7219 matrices ──────────────────────────
static void display_TC(int hour, int minute, int second, int frame) {

  int h0  = hour   % 10,  h1  = (hour   / 10) % 10;
  int m0  = minute % 10,  m1  = (minute / 10) % 10;
  int s0  = second % 10,  s1  = (second / 10) % 10;
  int f0  = frame  % 10,  f1  = (frame  / 10) % 10;

  unsigned char mtx[7][8] = { 0 };
  // device 0 = frames_units (rightmost)
  mtx[0][0]=font_number[10][0]; mtx[0][1]=font_number[f0][4];
  mtx[0][2]=font_number[f0][3]; mtx[0][3]=font_number[f0][2];
  mtx[0][4]=font_number[f0][1]; mtx[0][5]=font_number[f0][0];
  mtx[0][6]=font_number[10][0]; mtx[0][7]=font_number[f1][4];
  // device 1 = frames_tens + colon + sec_units start
  mtx[1][0]=font_number[f1][3]; mtx[1][1]=font_number[f1][2];
  mtx[1][2]=font_number[f1][1]; mtx[1][3]=font_number[f1][0];
  mtx[1][4]=font_number[10][0]; mtx[1][5]=font_number[4][1];   // colon dot
  mtx[1][6]=font_number[10][0]; mtx[1][7]=font_number[s0][4];
  // device 2 = seconds_units + seconds_tens start
  mtx[2][0]=font_number[s0][3]; mtx[2][1]=font_number[s0][2];
  mtx[2][2]=font_number[s0][1]; mtx[2][3]=font_number[s0][0];
  mtx[2][4]=font_number[10][0]; mtx[2][5]=font_number[s1][4];
  mtx[2][6]=font_number[s1][3]; mtx[2][7]=font_number[s1][2];
  // device 3 = seconds_tens + colon + minute_units start
  mtx[3][0]=font_number[s1][1]; mtx[3][1]=font_number[s1][0];
  mtx[3][2]=font_number[10][0]; mtx[3][3]=font_number[4][1];   // colon dot
  mtx[3][4]=font_number[10][0]; mtx[3][5]=font_number[m0][4];
  mtx[3][6]=font_number[m0][3]; mtx[3][7]=font_number[m0][2];
  // device 4 = minute_units + minute_tens
  mtx[4][0]=font_number[m0][1]; mtx[4][1]=font_number[m0][0];
  mtx[4][2]=font_number[10][0]; mtx[4][3]=font_number[m1][4];
  mtx[4][4]=font_number[m1][3]; mtx[4][5]=font_number[m1][2];
  mtx[4][6]=font_number[m1][1]; mtx[4][7]=font_number[m1][0];
  // device 5 = colon + hour_units start
  mtx[5][0]=font_number[10][0]; mtx[5][1]=font_number[4][1];   // colon dot
  mtx[5][2]=font_number[10][0]; mtx[5][3]=font_number[h0][4];
  mtx[5][4]=font_number[h0][3]; mtx[5][5]=font_number[h0][2];
  mtx[5][6]=font_number[h0][1]; mtx[5][7]=font_number[h0][0];
  // device 6 = hours_tens (leftmost)
  mtx[6][0]=font_number[10][0]; mtx[6][1]=font_number[h1][4];
  mtx[6][2]=font_number[h1][3]; mtx[6][3]=font_number[h1][2];
  mtx[6][4]=font_number[h1][1]; mtx[6][5]=font_number[h1][0];
  mtx[6][6]=font_number[10][0]; mtx[6][7]=font_number[10][0];

  // Rotate all matrices 90° CW
  for (int i = 0; i < 7; i++) rotateMatrixCW(mtx[i]);

  // Send to hardware
  for (int i = 0; i < 8; i++) {
    for (int d = 0; d < 7; d++) {
      matrix_setRow(d, i, mtx[d][i]);
    }
  }
}