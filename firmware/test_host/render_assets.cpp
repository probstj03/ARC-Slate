// Draws the e-ink bitmaps (boot picture and logo) with the real Adafruit_GFX, exactly like the device does.
#include <Adafruit_GFX.h>
#include <stdio.h>
#include <vector>
#include "../src/slate/assets/bootscreen.h"
#include "../src/slate/assets/logo.h"

class Surface : public Adafruit_GFX {
public:
    Surface() : Adafruit_GFX(800, 480), _px(800 * 480, 255) {}
    void drawPixel(int16_t x, int16_t y, uint16_t c) override {
        if (x >= 0 && y >= 0 && x < 800 && y < 480) _px[y * 800 + x] = c == 0 ? 0 : 255;
    }
    void save(const char* p) const { FILE* f = fopen(p, "wb"); fprintf(f, "P5\n800 480\n255\n"); fwrite(_px.data(), 1, _px.size(), f); fclose(f); }
private:
    std::vector<uint8_t> _px;
};

int main(int argc, char** argv) {
    Surface s;
    s.fillScreen(0xFFFF);
    s.drawInvertedBitmap(0, 0, bootscreen_V2, 800, 480, 0x0000);
    s.save(argc > 1 ? argv[1] : "boot.pgm");
    Surface t;
    t.fillScreen(0xFFFF);
    t.drawInvertedBitmap(344 + (456 - 170) / 2, 70, logo, 170, 170, 0x0000);
    t.save(argc > 2 ? argv[2] : "logo.pgm");
    printf("sizeof bootscreen %zu, logo %zu\n", sizeof(bootscreen_V2), sizeof(logo));
    return 0;
}
