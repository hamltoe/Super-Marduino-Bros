#include "Display.h"
#include "Game.h"
#include "World.h"
#include "Input.h"
#include <Adafruit_GFX.h>
#include <SPI.h>
#include <avr/pgmspace.h>

Adafruit_SSD1351 tft = Adafruit_SSD1351(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, CS_PIN, DC_PIN, RST_PIN);

// One world column: 128 pixels running from sky down to dirt.
static uint16_t colBuf[SCREEN_HEIGHT];

// composeColumn only touches rows inside this band, so a repaint
// that only needs Mario's slice does not pay for a full column.
static int16_t clipTop = 0;
static int16_t clipBot = SCREEN_HEIGHT - 1;

static int32_t panelCam = 0;
bool panelValid = false;
static int32_t marioColPrev = 0;
static int16_t marioRowPrev = 0;

// Disc half-widths: circTab[r][d] = floor(sqrt(r^2 - d^2)). Flash, not SRAM.
static const uint8_t circTab[12][12] PROGMEM = {
  { 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
  { 1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
  { 2,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
  { 3,  2,  2,  0,  0,  0,  0,  0,  0,  0,  0,  0},
  { 4,  3,  3,  2,  0,  0,  0,  0,  0,  0,  0,  0},
  { 5,  4,  4,  4,  3,  0,  0,  0,  0,  0,  0,  0},
  { 6,  5,  5,  5,  4,  3,  0,  0,  0,  0,  0,  0},
  { 7,  6,  6,  6,  5,  4,  3,  0,  0,  0,  0,  0},
  { 8,  7,  7,  7,  6,  6,  5,  3,  0,  0,  0,  0},
  { 9,  8,  8,  8,  8,  7,  6,  5,  4,  0,  0,  0},
  {10,  9,  9,  9,  9,  8,  8,  7,  6,  4,  0,  0},
  {11, 10, 10, 10, 10,  9,  9,  8,  7,  6,  4,  0},
};

static void vspan(uint16_t* b, int16_t y0, int16_t y1, uint16_t c) {
  if (y0 < clipTop) y0 = clipTop;
  if (y1 > clipBot) y1 = clipBot;
  if (y0 > y1) return;
  uint16_t* p = b + gramCol(y0, y1);
  for (int16_t n = y1 - y0 + 1; n > 0; n--) *p++ = c;
}

static void colDisc(uint16_t* b, int16_t du, uint8_t r, int16_t cy, uint16_t color) {
  if (du < 0) du = -du;
  if (du > r) return;
  uint8_t hh = pgm_read_byte(&circTab[r][du]);
  vspan(b, cy - hh, cy + hh, color);
}

static void colCloud(uint16_t* b, int16_t u, int16_t cy) {
  colDisc(b, u - 6, 5, cy + 5, WHITE);
  colDisc(b, u - 13, 7, cy + 2, WHITE);
  colDisc(b, u - 21, 5, cy + 5, WHITE);
  if (u >= 6 && u <= 21) vspan(b, cy + 5, cy + 10, WHITE);
}

// Apex at the top, base on the ground: the span starts T rows below
// the apex, where T is the first row wide enough to reach this column.
static void colHill(uint16_t* b, int16_t u, uint16_t color) {
  int16_t du = u - 25;
  if (du < 0) du = -du;
  int16_t t = (du * 6 + 4) / 5;
  if (t <= 30) vspan(b, GROUND_Y - 30 + t, GROUND_Y - 1, color);
  colDisc(b, du, 11, GROUND_Y - 19, color);
}

static void colBlock(uint16_t* b, int16_t u, int16_t by, bool question) {
  if (u == 0 || u == 14) {
    vspan(b, by, by + 14, DARK_DIRT);
    return;
  }

  vspan(b, by, by + 14, question ? YELLOW : ORANGE);
  vspan(b, by, by, DARK_DIRT);
  vspan(b, by + 14, by + 14, DARK_DIRT);
  if (u >= 2 && u <= 12) vspan(b, by + 2, by + 2, WHITE);

  if (question) {
    if (u >= 5 && u <= 9) vspan(b, by + 4, by + 5, DARK_DIRT);
    if (u >= 8 && u <= 9) vspan(b, by + 6, by + 8, DARK_DIRT);
    if (u >= 6 && u <= 7) {
      vspan(b, by + 9, by + 10, DARK_DIRT);
      vspan(b, by + 12, by + 13, DARK_DIRT);
    }
  } else {
    if (u == 7) vspan(b, by, by + 4, DARK_DIRT);
    if (u == 4) vspan(b, by + 9, by + 14, DARK_DIRT);
    vspan(b, by + 7, by + 7, DARK_DIRT);
  }
}

static void colPipe(uint16_t* b, int16_t u) {
  const int16_t top = GROUND_Y - 34;
  const int16_t capBot = GROUND_Y - 27;

  vspan(b, top, capBot, GREEN);
  if (u == 0 || u == 23) {
    vspan(b, top, capBot, DARK_DIRT);
  } else {
    vspan(b, top, top, DARK_DIRT);
    vspan(b, capBot, capBot, DARK_DIRT);
  }

  if (u >= 3 && u <= 20) {
    vspan(b, GROUND_Y - 26, GROUND_Y - 1, u == 20 ? DARK_DIRT : GREEN);
  }
  if (u == 6) vspan(b, GROUND_Y - 31, GROUND_Y - 1, PIPE_HI);
}

static void colCoin(uint16_t* b, int16_t u, int16_t cy) {
  colDisc(b, u - 4, 4, cy + 5, YELLOW);
  if (u == 4) vspan(b, cy + 2, cy + 7, WHITE);
}

// 3x5 digits, one byte per column, bit0 = top row. Gap column is empty.
static const uint8_t FONT3x5[] PROGMEM = {
  0x1F, 0x11, 0x1F, // 0
  0x00, 0x1F, 0x00, // 1
  0x1D, 0x15, 0x17, // 2
  0x15, 0x15, 0x1F, // 3
  0x07, 0x04, 0x1F, // 4
  0x17, 0x15, 0x1D, // 5
  0x1F, 0x15, 0x1D, // 6
  0x01, 0x01, 0x1F, // 7
  0x1F, 0x15, 0x1F, // 8
  0x17, 0x15, 0x1F, // 9
};

// One HUD digit column: 3x5 font scaled 2x (local 0..6, last is gap).
static void composeHudDigit(uint16_t* b, int16_t local, uint8_t dig, uint16_t color) {
  uint8_t px = (uint8_t)(local % HUD_DIGIT_W);
  if (px >= 6) return;
  uint8_t bits = pgm_read_byte(&FONT3x5[dig * 3 + (px >> 1)]);
  for (uint8_t row = 0; row < 5; row++) {
    if (bits & (1 << row)) {
      int16_t y = HUD_Y0 + (int16_t)(row << 1);
      vspan(b, y, y + 1, color);
    }
  }
}

// Digit idx from the left of a zero-padded n-digit value (no RAM cache).
static uint8_t nthDigit(uint16_t v, uint8_t idx, uint8_t n) {
  for (uint8_t i = (uint8_t)(n - 1); i > idx; i--) v /= 10;
  return (uint8_t)(v % 10);
}

// Screen-fixed score (left) and timer (right) in the sky strip.
static void composeHud(uint16_t* b, int32_t worldX) {
  int16_t sx = (int16_t)(worldX - cameraX);
  if (sx >= SCORE_HUD_X && sx < SCORE_HUD_X + SCORE_HUD_W) {
    int16_t local = sx - SCORE_HUD_X;
    composeHudDigit(b, local, nthDigit(score, local / HUD_DIGIT_W, 5), WHITE);
  } else if (sx >= TIME_HUD_X && sx < TIME_HUD_X + TIME_HUD_W) {
    int16_t local = sx - TIME_HUD_X;
    composeHudDigit(b, local, nthDigit(timeLeft, local / HUD_DIGIT_W, 3),
                    timeLeft <= 100 ? RED : WHITE);
  }
}

static void composeColumn(int32_t worldX, uint16_t* b) {
  vspan(b, 0, GROUND_Y - 1, SKY_BLUE);
  vspan(b, GROUND_Y, GROUND_Y + 3, GRASS);
  if ((worldX & 15) == 0) {
    vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, DARK_DIRT);
  } else {
    vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, DIRT);
    vspan(b, GROUND_Y + 15, GROUND_Y + 15, DARK_DIRT);
  }

  int32_t section = worldX / SECTION_W;
  int16_t wx = (int16_t)(worldX - section * SECTION_W);

  for (uint8_t i = 0; i < WORLD_COUNT; i++) {
    uint8_t type = pgm_read_byte(&WORLD[i].type);
    if ((type == O_BLOCK || type == O_COIN) && isBroken(section, i)) continue;

    int16_t u = wx - (int16_t)pgm_read_word(&WORLD[i].x);
    if (u < 0 || u >= (int16_t)objWidth(type)) continue;

    int16_t oy = pgm_read_byte(&WORLD[i].y);
    switch (type) {
      case O_CLOUD:  colCloud(b, u, oy); break;
      case O_HILL:   colHill(b, u, HILL_GREEN); break;
      case O_HILL2:  colHill(b, u, HILL_LIGHT); break;
      case O_COIN:   colCoin(b, u, oy); break;
      case O_PIPE:   colPipe(b, u); break;
      case O_QBLOCK: colBlock(b, u, oy, !isUsedQ(section, i)); break;
      default:       colBlock(b, u, oy, false); break;
    }
  }

  composeHud(b, worldX);
  if (!controllerOk && worldX < 24) vspan(b, 4, 10, RED);
}

// --- Enemy sprites --------------------------------------------------------

// One rectangle of a sprite, clipped to the column being composed.
// Calls paint in order, so a later part covers an earlier one.
static void spritePart(uint16_t* b, int16_t lu, int16_t top, int16_t lx, int16_t ly,
                int16_t w, int16_t h, int16_t sw, bool flip, uint16_t color) {
  int16_t x0 = flip ? (sw - lx - w) : lx;
  if (lu < x0 || lu >= x0 + w) return;
  vspan(b, top + ly, top + ly + h - 1, color);
}

static void colGoomba(uint16_t* b, int16_t u, int16_t top, uint8_t frame) {
  spritePart(b, u, top, 1, 0, 12, 10, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 3, 4, 2, 3, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 9, 4, 2, 3, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 4, 5, 1, 2, GOOMBA_W, false, BLACK);
  spritePart(b, u, top, 9, 5, 1, 2, GOOMBA_W, false, BLACK);
  int16_t fx = frame ? 2 : 0;
  spritePart(b, u, top, fx, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
  spritePart(b, u, top, 9 - fx, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
}

static void colSquashed(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 14, 4, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 0, 4, 14, 2, GOOMBA_W, false, GOOMBA_FT);
}

static void colShellBody(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 13, 9, KOOPA_W, false, SHELL_GRN);
  spritePart(b, u, top, 2, 2, 9, 5, KOOPA_W, false, SHELL_LT);
  spritePart(b, u, top, 0, 9, 13, 3, KOOPA_W, false, SHELL_RIM);
}

static void colKoopa(uint16_t* b, int16_t u, int16_t top, uint8_t frame, bool flip) {
  spritePart(b, u, top, 3, 0, 7, 7, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 6, 1, 3, 3, KOOPA_W, flip, WHITE);
  spritePart(b, u, top, 7, 2, 1, 2, KOOPA_W, flip, BLACK);
  colShellBody(b, u, top + 7);
  int16_t fx = frame ? 1 : 0;
  spritePart(b, u, top, fx, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 8 - fx, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
}

static void composeEnemies(uint16_t* b, int32_t worldX) {
  uint8_t frame = enemyFrame;

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE || e.state == ES_GONE) continue;

    int32_t left = e.xq >> 8;
    if (worldX < left || worldX >= left + enemyWidth(e)) continue;

    int16_t u = (int16_t)(worldX - left);
    int16_t top = (int16_t)(e.yq >> 8);

    if (e.type == E_GOOMBA) {
      if (e.state == ES_SQUASH) colSquashed(b, u, top);
      else                      colGoomba(b, u, top, frame);
    } else if (e.state == ES_WALK) {
      colKoopa(b, u, top, frame, e.vxq < 0);
    } else {
      colShellBody(b, u, top);
    }
  }
}

// --- Player ---------------------------------------------------------------

static void runnerPart(uint16_t* b, int16_t lu, int16_t py, int16_t lx, int16_t ly,
                int16_t w, int16_t h, uint16_t color) {
  spritePart(b, lu, py, lx, ly, w, h, PLAYER_W, !facingRight, color);
}

static void colMushroom(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 14, 7, MUSH_W, false, RED);
  spritePart(b, u, top, 3, 2, 2, 2, MUSH_W, false, WHITE);
  spritePart(b, u, top, 9, 3, 2, 2, MUSH_W, false, WHITE);
  spritePart(b, u, top, 4, 7, 6, 5, MUSH_W, false, SKIN);
  spritePart(b, u, top, 2, 12, 4, 2, MUSH_W, false, SKIN);
  spritePart(b, u, top, 8, 12, 4, 2, MUSH_W, false, SKIN);
}

static void composeItems(uint16_t* b, int32_t worldX) {
  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE || it.state == IS_GONE) continue;

    int32_t left = it.xq >> 8;
    if (worldX < left || worldX >= left + MUSH_W) continue;

    colMushroom(b, (int16_t)(worldX - left), (int16_t)(it.yq >> 8));
  }
}

static void composeRunner(uint16_t* b, int32_t worldX) {
  int32_t left = playerXq >> 8;
  if (worldX < left || worldX >= left + PLAYER_W) return;

  int16_t py = (int16_t)(playerYq >> 8);
  int16_t ph = playerH();
  // Off-screen during the death fall: skip draws (dirty rect still erases).
  if (py >= SCREEN_HEIGHT || py + ph <= 0) return;

  int16_t lu = (int16_t)(worldX - left);
  uint16_t accent = playAsLuigi ? LUIGI_GRN : RED;
  // Big Mario is small Mario stretched ~1.5x in Y (integer parts below).
  const bool big = bigMario;
  const int16_t hatH = big ? 3 : 2;
  const int16_t faceY = big ? 6 : 4;
  const int16_t faceH = big ? 5 : 4;
  const int16_t bodyY = big ? 11 : 8;
  const int16_t bodyH = big ? 7 : 3;
  const int16_t armY = big ? 12 : 8;
  const int16_t armH = big ? 5 : 3;
  const int16_t legY = big ? 18 : 11;
  const int16_t legH = big ? 4 : 3;

  runnerPart(b, lu, py, 3, 0, 8, hatH, accent);
  runnerPart(b, lu, py, 1, hatH, 12, hatH, accent);
  runnerPart(b, lu, py, 4, faceY, 7, faceH, SKIN);
  runnerPart(b, lu, py, 9, faceY + 1, 2, 2, DARK_DIRT);
  runnerPart(b, lu, py, 2, bodyY, 10, bodyH, BLUE);
  runnerPart(b, lu, py, 0, armY, 3, armH, accent);
  runnerPart(b, lu, py, 11, armY, 3, armH, accent);

  if (!onGround || playState == PLAY_DEATH_FALL) {
    runnerPart(b, lu, py, 1, legY, 4, legH, DARK_DIRT);
    runnerPart(b, lu, py, 9, legY, 4, legH, DARK_DIRT);
  } else if (animFrame == 0) {
    runnerPart(b, lu, py, 2, legY, 4, legH, DARK_DIRT);
    runnerPart(b, lu, py, 9, legY, 5, legH - 1, DARK_DIRT);
  } else {
    runnerPart(b, lu, py, 0, legY, 5, legH - 1, DARK_DIRT);
    runnerPart(b, lu, py, 9, legY, 4, legH, DARK_DIRT);
  }
}

static void pushColumn(int32_t worldX, int16_t y0, int16_t y1) {
  uint8_t c0 = gramCol(y0, y1);
  uint8_t len = (uint8_t)(y1 - y0 + 1);
  tft.setAddrWindow(c0, gramRow(worldX), len, 1);
  tft.writePixels(colBuf + c0, len);
#if DEBUG_SERIAL
  pixelCount += len;
#endif
}

static void paintColumn(int32_t worldX, int16_t y0, int16_t y1) {
  clipTop = y0;
  clipBot = y1;
  composeColumn(worldX, colBuf);
  composeEnemies(colBuf, worldX);
  composeItems(colBuf, worldX);
  composeRunner(colBuf, worldX);
  pushColumn(worldX, y0, y1);
}

static void setStartLine(int32_t cam) {
#if SCROLL_REVERSE
  // Column cam sits at row (128 - cam) and has to appear at the far
  // end of a reversed scan, which puts the origin one row past it.
  uint8_t v = (uint8_t)((129 - (cam & 127)) & 127);
#else
  uint8_t v = (uint8_t)(cam & 127);
#endif
  tft.sendCommand(SSD1351_CMD_STARTLINE, &v, 1);
}

// Dirty-rect paint for a moving actor. Returns true if the slot should free.
static bool paintActorRect(int32_t cam, int32_t& prevX, int16_t& prevY,
                    int32_t cx, int16_t cy, int16_t w, int16_t maxH, bool gone) {
  int32_t x0 = gone ? prevX : min(prevX, cx);
  int32_t x1 = (gone ? prevX : max(prevX, cx)) + w - 1;
  int16_t y0 = gone ? prevY : min(prevY, cy);
  int16_t y1 = (gone ? prevY : max(prevY, cy)) + maxH - 1;
  if (y0 < 0) y0 = 0;
  if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;

  for (int32_t wx = x0; wx <= x1; wx++) {
    if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
    paintColumn(wx, y0, y1);
  }

  prevX = cx;
  prevY = cy;
  return gone;
}

static void paintEnemyRects(int32_t cam) {
  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE) continue;

    int32_t cx = e.xq >> 8;
    int16_t cy = (int16_t)(e.yq >> 8);
    bool gone = (e.state == ES_GONE);

    // Parked shell / flattened Goomba: pixels already correct if still.
    if (!gone && cx == e.prevX && cy == e.prevY &&
        (e.state == ES_SHELL || e.state == ES_SQUASH)) {
      continue;
    }

    if (paintActorRect(cam, e.prevX, e.prevY, cx, cy,
                       enemyWidth(e), enemyMaxHeight(e), gone)) {
      e.type = E_NONE;
    }
  }
}

static void syncEnemyRects() {
  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE) continue;
    if (e.state == ES_GONE) {
      e.type = E_NONE;
      continue;
    }
    e.prevX = e.xq >> 8;
    e.prevY = (int16_t)(e.yq >> 8);
  }
}

static void paintItemRects(int32_t cam) {
  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE) continue;

    if (paintActorRect(cam, it.prevX, it.prevY,
                       it.xq >> 8, (int16_t)(it.yq >> 8),
                       MUSH_W, MUSH_H, it.state == IS_GONE)) {
      it.type = IT_NONE;
    }
  }
}

static void syncItemRects() {
  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE) continue;
    if (it.state == IS_GONE) {
      it.type = IT_NONE;
      continue;
    }
    it.prevX = it.xq >> 8;
    it.prevY = (int16_t)(it.yq >> 8);
  }
}

void render() {
  int32_t cam = cameraX;
  int32_t marioCol = playerXq >> 8;
  int16_t marioRow = (int16_t)(playerYq >> 8);
  int32_t dx = cam - panelCam;

  if (!panelValid || dx >= SCREEN_WIDTH || dx <= -SCREEN_WIDTH) {
    tft.startWrite();
    for (int16_t i = 0; i < SCREEN_WIDTH; i++) {
      paintColumn(cam + i, 0, SCREEN_HEIGHT - 1);
    }
    tft.endWrite();
    panelValid = true;
    pendingEraseCount = 0;
    syncEnemyRects();
    syncItemRects();
  } else {
    tft.startWrite();

    // Columns that just scrolled into view get full world content.
    if (dx > 0) {
      for (int32_t i = 0; i < dx; i++) {
        paintColumn(panelCam + SCREEN_WIDTH + i, 0, SCREEN_HEIGHT - 1);
      }
    } else if (dx < 0) {
      for (int32_t i = 0; i < -dx; i++) {
        paintColumn(cam + i, 0, SCREEN_HEIGHT - 1);
      }
    }

    // Everything Mario covered or now covers, repainted world-first
    // so his old pixels are erased in the same write. Use big height so
    // grow/shrink dirty rects always cover the taller pose.
    int32_t c0 = min(marioColPrev, marioCol);
    int32_t c1 = max(marioColPrev, marioCol) + PLAYER_W - 1;
    int16_t y0 = min(marioRowPrev, marioRow);
    int16_t y1 = max(marioRowPrev, marioRow) + PLAYER_H_BIG - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;

    for (int32_t wx = c0; wx <= c1; wx++) {
      if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
      paintColumn(wx, y0, y1);
    }

    // One-shot: rewrite busted brick / used-? columns so GRAM matches
    // composeColumn. brokenBricks / usedQBlocks keep later dirties correct.
    for (uint8_t e = 0; e < pendingEraseCount; e++) {
      EraseRect& r = pendingErase[e];
      int16_t y1e = r.y + r.h - 1;
      if (r.y < 0) continue;
      if (y1e > SCREEN_HEIGHT - 1) y1e = SCREEN_HEIGHT - 1;
      for (int32_t wx = r.x; wx < r.x + r.w; wx++) {
        if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
        paintColumn(wx, r.y, y1e);
      }
    }
    pendingEraseCount = 0;

    paintEnemyRects(cam);
    paintItemRects(cam);

    // Screen-fixed HUD: repaint the union of last and current strips so
    // columns that scrolled out of the band are rewritten as plain sky
    // (composeHud only stamps digits at the current screen X).
    {
      int32_t s0 = min(panelCam + SCORE_HUD_X, cam + SCORE_HUD_X);
      int32_t s1 = max(panelCam + SCORE_HUD_X, cam + SCORE_HUD_X) + SCORE_HUD_W - 1;
      for (int32_t wx = s0; wx <= s1; wx++) {
        if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
        paintColumn(wx, 0, HUD_Y1);
      }
      int32_t t0 = min(panelCam + TIME_HUD_X, cam + TIME_HUD_X);
      int32_t t1 = max(panelCam + TIME_HUD_X, cam + TIME_HUD_X) + TIME_HUD_W - 1;
      for (int32_t wx = t0; wx <= t1; wx++) {
        if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
        paintColumn(wx, 0, HUD_Y1);
      }
    }

    tft.endWrite();
  }

  setStartLine(cam);
  panelCam = cam;
  marioColPrev = marioCol;
  marioRowPrev = marioRow;
}

void applyGameRemap() {
  // Gameplay owns the panel: rotation 0 + custom remap for column scroll.
  tft.setRotation(0);
  uint8_t remap = 0b01100100;  // no bottom-up scan
  tft.sendCommand(SSD1351_CMD_SETREMAP, &remap, 1);
  uint8_t zero = 0;
  tft.sendCommand(SSD1351_CMD_STARTLINE, &zero, 1);
}

void applyUiRemap() {
  tft.setRotation(UI_ROTATION);
  tft.setTextWrap(false);
}
