#pragma once
// ARC-Slate pin map on the ARC_AP board (ESP32-S3-DevKitC-1 N16R8).
// Wiring diagram: hardware/wiring/ARC-Slate_Verdrahtung.svg
// GPIO35–37 are used by the octal PSRAM and must not be used.

// On-board: PCM5102A (LTC output)
constexpr int PIN_DAC_BCK  = 18;
constexpr int PIN_DAC_LRCK = 17;
constexpr int PIN_DAC_DIN  = 16;
constexpr int PIN_PCM_XSMT = 3;   // HIGH = output, LOW = soft mute

// On-board: LTC input stage (LM358 + 74HC14)
constexpr int PIN_LTC_IN = 4;

// U2: DS3231 RTC
constexpr int PIN_RTC_SDA = 42;
constexpr int PIN_RTC_SCL = 41;
constexpr int PIN_RTC_SQW = 5;

// U4 (former TFT header): shared SPI bus for e-ink + both MAX7219 chains
constexpr int PIN_SPI_MOSI      = 11;  // U4 pin 6
constexpr int PIN_SPI_SCK       = 12;  // U4 pin 7
constexpr int PIN_EPD_CS        = 10;  // U4 pin 3
constexpr int PIN_EPD_RST       = 8;   // U4 pin 4
constexpr int PIN_EPD_DC        = 9;   // U4 pin 5
constexpr int PIN_EPD_BUSY      = 13;  // U4 pin 9
constexpr int PIN_MTX_TC_LOAD   = 7;   // U4 pin 8 – timecode chain (7 modules)

// J1 (former SD header)
constexpr int PIN_MTX_DATA_LOAD = 14;  // J1 pin 3 – data chain (9 modules)
constexpr int PIN_HALL          = 2;   // J1 pin 4 – hall sensor, LOW = slate closed
constexpr int PIN_FLASH         = 15;  // J1 pin 5 – LD06AJSA DIM/EN, HIGH = flash on

// SW1: rotary encoder KY-040
constexpr int PIN_ENC_SW  = 40;  // SW1 pin 3
constexpr int PIN_ENC_DT  = 39;  // SW1 pin 4
constexpr int PIN_ENC_CLK = 38;  // SW1 pin 5
