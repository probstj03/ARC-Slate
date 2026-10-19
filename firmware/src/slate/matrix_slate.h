#pragma once
// What the slate shows on the LED matrices: timecode (TC chain) and roll / shot /
// scene + take (data chain). Registers itself as the renderer of the matrix driver.
// The brightness follows the sun (Settings: brightness by day, brightnessNight).
namespace slatematrix {
void begin();
bool isNight();     // between sunset and sunrise: the menu / `bright N` change the night value
}
