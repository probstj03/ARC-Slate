// Renders the weather animations of the ARC-Clock data chain (72x8 LEDs) as a contact sheet:
// one row per weather kind (day, then night for clear / partly cloudy), 12 frames side by side.
//   render_anim out.pgm
#include <stdio.h>
#include <vector>
#include "anim.h"

using namespace ac;

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "anim.pgm";
    struct Row { WxKind kind; bool day; };
    const Row rows[] = {{WxKind::Clear, true}, {WxKind::Clear, false}, {WxKind::PartlyCloudy, true}, {WxKind::PartlyCloudy, false},
                        {WxKind::Cloudy, true}, {WxKind::Fog, true}, {WxKind::Drizzle, true}, {WxKind::Rain, true},
                        {WxKind::Showers, true}, {WxKind::Snow, true}, {WxKind::Thunder, true}, {WxKind::Unknown, true}};
    const int nRows = sizeof(rows) / sizeof(rows[0]);
    const int frames = 4, step = 6;             // one frame per second (6 frames/s on the device)
    const int px = 7, gapX = 14, gapY = 10;      // LED size, gaps between pictures
    const int cellW = kWideW * px + gapX, cellH = 8 * px + gapY;
    const int W = frames * cellW, H = nRows * cellH;
    std::vector<uint8_t> img(W * H, 255);
    for (int r = 0; r < nRows; r++)
        for (int f = 0; f < frames; f++) {
            WideCanvas c;

            drawWeatherWide(c, rows[r].kind, rows[r].day, (uint32_t)(f * step));
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < c.w; x++) {
                    const uint8_t v = c.get(x, y) ? 0 : 225;     // lit LED black, dark LED light grey
                    for (int dy = 0; dy < px - 1; dy++)
                        for (int dx = 0; dx < px - 1; dx++) img[(r * cellH + y * px + dy) * W + f * cellW + x * px + dx] = v;
                }
        }
    FILE* fo = fopen(out, "wb");
    if (!fo) return 1;
    fprintf(fo, "P5\n%d %d\n255\n", W, H);
    fwrite(img.data(), 1, img.size(), fo);
    fclose(fo);
    printf("wrote %s\n", out);
    return 0;
}
