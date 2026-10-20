// Renders the night icons of the ARC-Clock matrix (moon of the real phase + permanent stars that grow into crosses)
// as a contact sheet: rows = phases (new, waxing, full, waning), columns = times (stars change every 4 s).
//   render_night out.pgm
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "anim.h"
#include "../src/clock/assets/weather_icons_night.h"

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "night.pgm";
    const double phases[] = {0.04, 0.15, 0.25, 0.33, 0.5, 0.67, 0.75, 0.9};
    const int nPhases = sizeof(phases) / sizeof(phases[0]);
    const int times = 3, px = 5, gap = 12;
    const int cellW = 24 * px + gap, cellH = 17 * px + gap;       // the gap between row 7 and 8 is drawn
    const int blocks = 2;                                          // clear night, heiter nacht
    const int W = nPhases * cellW, H = blocks * times * cellH;
    std::vector<uint8_t> img(W * H, 255);
    for (int b = 0; b < blocks; b++)
        for (int t = 0; t < times; t++)
            for (int p = 0; p < nPhases; p++) {
                uint8_t icon[48];
                if (b == 0) ac::composeNightIcon(icon, anim16_clear_night_base[0], NIGHT_CLEAR_MOON_X, night_clear_stars, NIGHT_CLEAR_STAR_N, phases[p], t * 4000);
                else ac::composeNightIcon(icon, anim16_heiter_nacht_base[0], NIGHT_HEITER_MOON_X, night_heiter_stars, NIGHT_HEITER_STAR_N, phases[p], t * 4000);
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 24; x++) {
                        const uint8_t v = ((icon[y * 3 + x / 8] >> (7 - x % 8)) & 1) ? 0 : 225;
                        const int yy = y * px + (y >= 8 ? px : 0);
                        for (int dy = 0; dy < px - 1; dy++)
                            for (int dx = 0; dx < px - 1; dx++)
                                img[((b * times + t) * cellH + yy + dy) * W + p * cellW + x * px + dx] = v;
                    }
            }
    FILE* f = fopen(out, "wb");
    if (!f) return 1;
    fprintf(f, "P5\n%d %d\n255\n", W, H);
    fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    printf("wrote %s\n", out);
    return 0;
}
