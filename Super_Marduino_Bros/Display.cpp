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
static const uint8_t circTab[5][5] PROGMEM = {
  { 0,  0,  0,  0,  0},
  { 1,  0,  0,  0,  0},
  { 2,  1,  0,  0,  0},
  { 3,  2,  2,  0,  0},
  { 4,  3,  3,  2,  0},
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
  colDisc(b, u - 6, 4, cy + 5, WHITE);
  colDisc(b, u - 13, 4, cy + 2, WHITE);
  colDisc(b, u - 21, 4, cy + 5, WHITE);
}

static void colHill(uint16_t* b, int16_t u, uint16_t color) {
  int16_t du = u - 25;
  if (du < 0) du = -du;
  int16_t t = (du * 6 + 4) / 5;
  if (t <= 30) vspan(b, GROUND_Y - 30 + t, GROUND_Y - 1, color);
}

static void colBlock(uint16_t* b, int16_t u, int16_t by, bool question) {
  uint16_t fill = question ? YELLOW : palBrick;
  uint16_t edge = palBrickDk;
  if (u == 0 || u == 14) {
    vspan(b, by, by + 14, edge);
    return;
  }

  vspan(b, by, by + 14, fill);
  vspan(b, by, by, edge);
  vspan(b, by + 14, by + 14, edge);
  if (u >= 2 && u <= 12) vspan(b, by + 2, by + 2, WHITE);

  if (question) {
    if (u >= 5 && u <= 9) vspan(b, by + 4, by + 5, edge);
    if (u >= 8 && u <= 9) vspan(b, by + 6, by + 8, edge);
    if (u >= 6 && u <= 7) {
      vspan(b, by + 9, by + 10, edge);
      vspan(b, by + 12, by + 13, edge);
    }
  } else {
    if (u == 7) vspan(b, by, by + 4, edge);
    if (u == 4) vspan(b, by + 9, by + 14, edge);
    vspan(b, by + 7, by + 7, edge);
  }
}

static void colFlag(uint16_t* b, int16_t u, int16_t top) {
  if (u <= 2) vspan(b, top + 4, top + 71, u == 1 ? GREEN : WHITE);
  if (u <= 2) colDisc(b, u - 1, 2, top + 2, WHITE);
  if (u >= 3 && u <= 12) {
    int16_t drop = (int16_t)(u - 3);
    vspan(b, top + 6 + drop, top + 18 - drop, RED);
  }
}

static void colAxe(uint16_t* b, int16_t u, int16_t top) {
  if (u == 4 || u == 5) vspan(b, top + 4, top + 14, DARK_DIRT);
  if (u <= 9) vspan(b, top, top + 4, palBrick);
}

static void colPlat(uint16_t* b, int16_t u, int16_t by) {
  uint16_t fill = (levelTheme == TH_MUSH) ? RED : palBrick;
  vspan(b, by, by + 7, fill);
  if (u == 0 || u == 39) vspan(b, by, by + 7, WHITE);
}

static void colBeam(uint16_t* b, int16_t u, int16_t by, uint8_t w) {
  vspan(b, by, by + BEAM_H - 1, ORANGE);
  if (u == 0 || u == (int16_t)(w - 1)) vspan(b, by, by + BEAM_H - 1, WHITE);
}

static void colCoin(uint16_t* b, int16_t u, int16_t cy) {
  colDisc(b, u - 4, 4, cy + 5, YELLOW);
  if (u == 4) vspan(b, cy + 2, cy + 7, WHITE);
}

static void composeColumn(int32_t worldX, uint16_t* b) {
  if (levelFlags & LF_PITS) {
    vspan(b, 0, SCREEN_HEIGHT - 1, palSky);
    if (levelFlags & LF_LAVA) {
      vspan(b, LAVA_Y, SCREEN_HEIGHT - 1, (worldX & 4) ? RED : ORANGE);
    }
  } else {
    vspan(b, 0, GROUND_Y - 1, palSky);
    vspan(b, GROUND_Y, GROUND_Y + 3, palGrass);
    if ((worldX & 15) == 0) {
      vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, palBrickDk);
    } else {
      vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, palDirt);
      vspan(b, GROUND_Y + 15, GROUND_Y + 15, palBrickDk);
    }
  }

  // Past the course: sky + ground only (no repeating scenery).
  if (worldX < 0 || worldX >= levelW) return;

  int16_t wx = (int16_t)worldX;

  for (uint8_t i = 0; i < worldCount; i++) {
    uint8_t type = pgm_read_byte(&worldObjs[i].type);
    if ((type == O_BLOCK || type == O_COIN) && isBroken(0, i)) continue;

    int16_t u = wx - (int16_t)pgm_read_word(&worldObjs[i].x);
    if (u < 0 || u >= (int16_t)objWidth(type)) continue;

    int16_t oy = pgm_read_byte(&worldObjs[i].y);
    switch (type) {
      case O_CLOUD:  colCloud(b, u, oy); break;
      case O_HILL:   colHill(b, u, HILL_GREEN); break;
      case O_HILL2:  colHill(b, u, HILL_LIGHT); break;
      case O_COIN:   colCoin(b, u, oy); break;
      case O_PLAT:   colPlat(b, u, oy); break;
      case O_FLAG:
        if (levelTheme == TH_CASTLE) colAxe(b, u, oy);
        else colFlag(b, u, oy);
        break;
      case O_QBLOCK: colBlock(b, u, oy, !isUsedQ(0, i)); break;
      default:       colBlock(b, u, oy, false); break;
    }
  }

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

static void colFish(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 2, 1, 10, 8, FISH_W, false, RED);
  spritePart(b, u, top, 0, 3, 3, 4, FISH_W, false, ORANGE);
}

static void colBowser(uint16_t* b, int16_t u, int16_t top, bool flip) {
  spritePart(b, u, top, 4, 0, 12, 8, BOWSER_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 2, 8, 16, 10, BOWSER_W, flip, GREEN);
  spritePart(b, u, top, 6, 10, 8, 4, BOWSER_W, flip, ORANGE);
  spritePart(b, u, top, 2, 18, 6, 4, BOWSER_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 12, 18, 6, 4, BOWSER_W, flip, KOOPA_SKIN);
}

static void composeBeams(uint16_t* b, int32_t worldX) {
  for (uint8_t i = 0; i < beamCount; i++) {
    Beam& beam = beams[i];
    if (worldX < beam.x || worldX >= beam.x + beam.w) continue;
    colBeam(b, (int16_t)(worldX - beam.x), beam.y, beam.w);
  }
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
    } else if (e.type == E_FISH) {
      colFish(b, u, top);
    } else if (e.type == E_BOWSER) {
      colBowser(b, u, top, e.vxq < 0);
    }
  }
}

// --- Player ---------------------------------------------------------------

static void runnerPart(uint16_t* b, int16_t lu, int16_t py, int16_t lx, int16_t ly,
                int16_t w, int16_t h, uint16_t color) {
  spritePart(b, lu, py, lx, ly, w, h, PLAYER_W, !facingRight, color);
}

static void composeRunner(uint16_t* b, int32_t worldX) {
  int32_t left = playerXq >> 8;
  if (worldX < left || worldX >= left + PLAYER_W) return;

  int16_t py = (int16_t)(playerYq >> 8);
  int16_t ph = PLAYER_H_SMALL;
  if (py >= SCREEN_HEIGHT || py + ph <= 0) return;

  int16_t lu = (int16_t)(worldX - left);
  uint16_t accent = playAsLuigi ? LUIGI_GRN : RED;

  runnerPart(b, lu, py, 3, 0, 8, 2, accent);
  runnerPart(b, lu, py, 1, 2, 12, 2, accent);
  runnerPart(b, lu, py, 4, 4, 7, 4, SKIN);
  runnerPart(b, lu, py, 9, 5, 2, 2, DARK_DIRT);
  runnerPart(b, lu, py, 2, 8, 10, 3, BLUE);
  runnerPart(b, lu, py, 0, 8, 3, 3, accent);
  runnerPart(b, lu, py, 11, 8, 3, 3, accent);

  if (!onGround || playState == PLAY_DEATH_FALL) {
    runnerPart(b, lu, py, 1, 11, 4, 3, DARK_DIRT);
    runnerPart(b, lu, py, 9, 11, 4, 3, DARK_DIRT);
  } else if (animFrame == 0) {
    runnerPart(b, lu, py, 2, 11, 4, 3, DARK_DIRT);
    runnerPart(b, lu, py, 9, 11, 5, 2, DARK_DIRT);
  } else {
    runnerPart(b, lu, py, 0, 11, 5, 2, DARK_DIRT);
    runnerPart(b, lu, py, 9, 11, 4, 3, DARK_DIRT);
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
  composeBeams(colBuf, worldX);
  composeEnemies(colBuf, worldX);
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
        (e.state == ES_SQUASH)) {
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

static void paintBeamRects(int32_t cam) {
  for (uint8_t i = 0; i < beamCount; i++) {
    Beam& b = beams[i];
    int32_t x0 = (b.prevX < b.x) ? b.prevX : b.x;
    int32_t x1 = ((b.prevX > b.x) ? b.prevX : b.x) + b.w - 1;
    int16_t y0 = b.y;
    int16_t y1 = (int16_t)(b.y + BEAM_H - 1);
    for (int32_t wx = x0; wx <= x1; wx++) {
      if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
      paintColumn(wx, y0, y1);
    }
    b.prevX = b.x;
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
    for (uint8_t i = 0; i < beamCount; i++) beams[i].prevX = beams[i].x;
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
    int16_t y1 = max(marioRowPrev, marioRow) + PLAYER_H_SMALL - 1;
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
    paintBeamRects(cam);

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
