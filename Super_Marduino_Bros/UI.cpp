#include "UI.h"
#include "Display.h"
#include "Game.h"
#include "World.h"
#include "Input.h"
#include "Audio.h"

void enterMode(uint8_t mode) {
  gameMode = mode;
  modeMs = millis();
  uiDirty = true;
  // Pad lines often read "pressed" on boot / mode change; wait for a
  // clean release before Start/A can advance the menu again.
  menuArmed = false;

  if (mode == MODE_TITLE) {
    score = 0;
    levelIdx = 0;
    audioStopBgm();
  }

  if (mode == MODE_PLAY) {
    applyGameRemap();
    resetLevel();
    lastStep = millis();
    audioStartBgm();
  } else {
    applyUiRemap();
    if (mode != MODE_TITLE) audioStopBgm();
  }
}

static void uiFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  tft.fillRect(x, y, w, h, color);
}

static void uiClear() {
  tft.fillScreen(BLACK);
}

uint8_t uiStrLen_P(const __FlashStringHelper* fs) {
  const char* s = (const char*)fs;
  uint8_t n = 0;
  while (pgm_read_byte(s++)) n++;
  return n;
}

static void uiPrint_P(int16_t x, int16_t y, const __FlashStringHelper* fs,
               uint16_t color, uint8_t size) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(fs);
}

static void uiCenter(int16_t y, const __FlashStringHelper* s, uint16_t color, uint8_t size) {
  int16_t x = (SCREEN_WIDTH - (int16_t)uiStrLen_P(s) * 6 * size) / 2;
  uiPrint_P(x, y, s, color, size);
}

static void uiMenuCursor(int16_t x, int16_t y, bool on) {
  if (on) uiFill(x, y + 2, 3, 2, YELLOW), uiFill(x + 3, y + 1, 3, 4, YELLOW);
}

static void uiRunnerSwatch(int16_t x, int16_t y, bool luigi) {
  uint16_t accent = luigi ? LUIGI_GRN : RED;
  uiFill(x + 3, y, 8, 3, accent);
  uiFill(x + 1, y + 3, 12, 3, accent);
  uiFill(x + 4, y + 6, 7, 5, SKIN);
  uiFill(x + 2, y + 11, 10, 7, BLUE);
  uiFill(x, y + 12, 3, 5, accent);
  uiFill(x + 11, y + 12, 3, 5, accent);
  uiFill(x + 2, y + 18, 4, 4, DARK_DIRT);
  uiFill(x + 9, y + 18, 4, 4, DARK_DIRT);
}

static void uiMenuRow(int16_t y, const __FlashStringHelper* label, uint16_t color,
               bool selected, bool luigiSwatch) {
  uiMenuCursor(8, y + 8, selected);
  uiRunnerSwatch(22, y, luigiSwatch);
  uiPrint_P(44, y + 8, label, selected ? color : UI_DIM, 1);
  if (selected) uiFill(44, y + 18, 36, 1, color);
}

static void drawTitle() {
  uiClear();
  uiCenter(18, F("SUPER"), WHITE, 2);
  uiCenter(38, F("MARDUINO"), RED, 2);
  uiCenter(58, F("BROS"), WHITE, 2);
  uiMenuCursor(22, 88, true);
  uiPrint_P(34, 88, F("PRESS START"), YELLOW, 1);
  if (!controllerOk) uiCenter(108, F("NO PAD"), RED, 1);
}

static void drawSelect() {
  uiClear();
  uiCenter(8, F("SELECT PLAYER"), WHITE, 1);
  uiMenuRow(36, F("MARIO"), RED, selectIdx == 0, false);
  uiMenuRow(72, F("LUIGI"), LUIGI_GRN, selectIdx == 1, true);
  uiCenter(112, F("START TO CONFIRM"), UI_DIM, 1);
}

static void drawLives() {
  uiClear();
  char world[4] = {'1', '-', (char)('1' + levelIdx), '\0'};
  tft.setTextSize(1);
  tft.setTextColor(WHITE);
  tft.setCursor((SCREEN_WIDTH - 18) / 2, 14);
  tft.print(world);
  if (playAsLuigi) uiCenter(36, F("LUIGI"), LUIGI_GRN, 2);
  else             uiCenter(36, F("MARIO"), RED, 2);
  char line[4] = {'x', ' ', (char)('0' + lives), '\0'};
  tft.setTextSize(2);
  tft.setTextColor(WHITE);
  tft.setCursor((SCREEN_WIDTH - 36) / 2, 68);
  tft.print(line);
}

static void drawDead() {
  uiClear();
  uiCenter(40, F("GAME OVER"), RED, 2);
  uiMenuCursor(22, 78, true);
  uiPrint_P(34, 78, F("PRESS START"), YELLOW, 1);
}

static void drawWin() {
  uiClear();
  uiCenter(44, F("CLEAR!"), GREEN, 2);
  uiMenuCursor(22, 78, true);
  uiPrint_P(34, 78, F("PRESS START"), YELLOW, 1);
}

static void paintUiIfDirty(void (*draw)()) {
  if (!uiDirty) return;
  draw();
  uiDirty = false;
}

static bool menuConfirm(bool eStart, bool eA, bool startDown, bool aDown) {
  if (!menuArmed) {
    if (!startDown && !aDown) menuArmed = true;
    return false;
  }
  return eStart || eA;
}

void updateTitle(bool eStart, bool eA, bool startDown, bool aDown) {
  paintUiIfDirty(drawTitle);
  if (menuConfirm(eStart, eA, startDown, aDown)) {
    selectIdx = playAsLuigi ? 1 : 0;
    audioPlay(SFX_BLIP);
    enterMode(MODE_SELECT);
  }
}

void updateSelect(bool eStart, bool eA, bool eLeft, bool eRight,
                  bool eUp, bool eDown, bool startDown, bool aDown) {
  if (menuArmed) {
    if (eLeft || eUp) {
      if (selectIdx != 0) audioPlay(SFX_BLIP);
      selectIdx = 0;
      uiDirty = true;
    } else if (eRight || eDown) {
      if (selectIdx != 1) audioPlay(SFX_BLIP);
      selectIdx = 1;
      uiDirty = true;
    }
  }

  paintUiIfDirty(drawSelect);

  if (menuConfirm(eStart, eA, startDown, aDown)) {
    playAsLuigi = (selectIdx == 1);
    lives = START_LIVES;
    audioPlay(SFX_BLIP);
    enterMode(MODE_LIVES);
  }
}

void updateLives(bool eStart, bool startDown, bool aDown) {
  paintUiIfDirty(drawLives);
  bool timedOut = (millis() - modeMs) >= LIVES_HOLD_MS;
  if (timedOut || menuConfirm(eStart, false, startDown, aDown)) {
    enterMode(MODE_PLAY);
  }
}

void updateDead(bool eStart, bool startDown, bool aDown) {
  paintUiIfDirty(drawDead);
  if (menuConfirm(eStart, false, startDown, aDown)) {
    audioPlay(SFX_BLIP);
    enterMode(MODE_TITLE);
  }
}

void updateWin(bool eStart, bool startDown, bool aDown) {
  paintUiIfDirty(drawWin);
  if (menuConfirm(eStart, false, startDown, aDown)) {
    audioPlay(SFX_BLIP);
    enterMode(MODE_TITLE);
  }
}
