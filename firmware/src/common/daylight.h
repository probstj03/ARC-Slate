#pragma once
// Sunrise / sunset computed locally (NOAA approximation, about +-2 min) and the matrix brightness that follows
// them. Common to ARC-Slate and ARC-Clock: works without internet. The place is the one of ARC-Clock
// (NVS namespace "clock", lat/lon; set there with `loc`), default Poppenricht.
#include <stdint.h>

namespace daylight {

// Sunrise and sunset of a date in minutes after local midnight. false: polar day / night.
bool sunTimes(int year, int month, int day, double lat, double lon, int utcOffsetMin, int& riseMin, int& setMin);

// Central European time: +60 min, +120 min during summer time (last Sunday of March .. last Sunday of October).
int cetOffsetMin(int year, int month, int day, int hourLocal);

// Brightness for a minute of the day: `night` before sunrise / after sunset, `day` in between, linear over
// TWILIGHT_MIN around both (no jump).
constexpr int TWILIGHT_MIN = 30;
uint8_t level(int minuteOfDay, int riseMin, int setMin, uint8_t day, uint8_t night);

// The place from NVS (ARC-Clock settings); falls back to Poppenricht.
void place(double& lat, double& lon);

}  // namespace daylight
