#ifndef SUPER_MARDUINO_DISPLAY_H
#define SUPER_MARDUINO_DISPLAY_H

#include "Config.h"
#include <Adafruit_SSD1351.h>

extern Adafruit_SSD1351 tft;

// Render-owned GRAM / dirty-rect state (resetLevel clears panelValid).
extern bool panelValid;

// Game row -> GRAM column. A solid span is the same span reversed,
// so the mirror costs nothing but an index.
inline uint8_t gramCol(int16_t y0, int16_t y1) {
#if FLIP_Y
  (void)y0;
  return (uint8_t)(SCREEN_HEIGHT - 1 - y1);
#else
  (void)y1;
  return (uint8_t)y0;
#endif
}

// World column -> GRAM row.
inline uint8_t gramRow(int32_t worldX) {
#if SCROLL_REVERSE
  return (uint8_t)((128 - (worldX & 127)) & 127);
#else
  return (uint8_t)(worldX & 127);
#endif
}

void applyGameRemap();
void applyUiRemap();
void render();

#endif
