#include "Game.h"
#include "World.h"
#include "UI.h"
#include "Audio.h"
#include <avr/pgmspace.h>

// Owned by Display.cpp; cleared when a level boots so GRAM refills.
extern bool panelValid;

static void forceDeath();

Enemy enemies[MAX_ENEMIES];

uint16_t score = 0;

int32_t spawnFrontier = 0;
uint8_t animTick = 0;
uint8_t enemyFrame = 0;

int32_t playerXq = (int32_t)16 << 8;
int32_t playerYq = (int32_t)(GROUND_Y - PLAYER_H_SMALL) << 8;
int16_t velXq = 0;
int16_t velYq = 0;
bool onGround = true;
bool facingRight = true;
bool jumpWasHeld = false;
uint8_t animFrame = 0;
int32_t cameraX = 0;

uint8_t gameMode = MODE_TITLE;
uint8_t playState = PLAY_RUN;
uint8_t lives = START_LIVES;
bool playAsLuigi = false;
uint8_t selectIdx = 0;   // 0 = Mario, 1 = Luigi
uint32_t modeMs = 0;
uint32_t deathMs = 0;
bool deathNeedsRender = false;
bool uiDirty = true;
bool menuArmed = false;  // ignore confirm until Start/A have been released

bool prevStart = false;
bool prevA = false;
bool prevLeft = false;
bool prevRight = false;
bool prevUp = false;
bool prevDown = false;

uint32_t lastStep = 0;
#if DEBUG_SERIAL
uint32_t lastReport = 0;
uint16_t frameCount = 0;
uint32_t pixelCount = 0;
#endif

static void bustBrick(int32_t section, uint8_t index) {
  markGone(section, index, O_BLOCK);
  audioPlay(SFX_BREAK);
}

static void collectCoin(int32_t section, uint8_t index) {
  if (isBroken(section, index)) return;
  markGone(section, index, O_COIN);
  if (score <= 65535 - COIN_POINTS) score += COIN_POINTS;
  else score = 65535;
  audioPlay(SFX_COIN);
}

// Empty the ? block (stays solid) and pay out a coin once.
static void hitQBlock(int32_t section, uint8_t index) {
  if (isUsedQ(section, index)) return;

  if (usedQCount < MAX_USED_Q) {
    usedQBlocks[usedQCount].section = (int16_t)section;
    usedQBlocks[usedQCount].index = index;
    usedQCount++;
  } else {
    for (uint8_t i = 1; i < MAX_USED_Q; i++) usedQBlocks[i - 1] = usedQBlocks[i];
    usedQBlocks[MAX_USED_Q - 1].section = (int16_t)section;
    usedQBlocks[MAX_USED_Q - 1].index = index;
  }

  queueBlockErase(section, index, O_QBLOCK);
  if (score <= 65535 - COIN_POINTS) score += COIN_POINTS;
  else score = 65535;
  audioPlay(SFX_COIN);
}

static void hitLift(int32_t xq, int32_t yq, int32_t wq, int32_t hq,
                    int32_t sx, int16_t sy, int16_t sw, int16_t sh,
                    bool& hit, int32_t& lift) {
  int32_t sxq = sx << 8;
  int32_t syq = (int32_t)sy << 8;
  int32_t swq = (int32_t)sw << 8;
  int32_t shq = (int32_t)sh << 8;
  if (xq >= sxq + swq || xq + wq <= sxq) return;
  if (yq >= syq + shq || yq + hq <= syq) return;
  hit = true;
  int32_t up = (yq + hq) - syq;
  if (up > lift) lift = up;
}

static bool boxVsSolids(int32_t xq, int32_t yq, int32_t wq, int32_t hq, int32_t* liftOut) {
  int16_t px = (int16_t)(xq >> 8);
  bool hit = false;
  int32_t lift = 0;

  for (uint8_t i = 0; i < worldCount; i++) {
    uint8_t type = pgm_read_byte(&worldObjs[i].type);
    if (!objSolid(type)) continue;
    if (type == O_BLOCK && isBroken(0, i)) continue;

    int32_t sx = (int16_t)pgm_read_word(&worldObjs[i].x);
    int16_t sw = objWidth(type);
    int32_t gap = sx - px;
    if (gap > (wq >> 8) || gap < -sw) continue;

    hitLift(xq, yq, wq, hq, sx, pgm_read_byte(&worldObjs[i].y),
            sw, objHeight(type), hit, lift);
  }

  for (uint8_t b = 0; b < beamCount; b++) {
    int32_t sx = beams[b].x;
    int16_t sw = beams[b].w;
    int32_t gap = sx - px;
    if (gap > (wq >> 8) || gap < -sw) continue;
    hitLift(xq, yq, wq, hq, sx, beams[b].y, sw, BEAM_H, hit, lift);
  }

  if (liftOut) *liftOut = lift;
  return hit;
}

static void resolveBox(int32_t sx, int16_t sy, int16_t sw, int16_t sh,
                       bool rising, int32_t section, uint8_t index, uint8_t type) {
  int32_t sxq = sx << 8;
  int32_t syq = (int32_t)sy << 8;
  int32_t swq = (int32_t)sw << 8;
  int32_t shq = (int32_t)sh << 8;
  int32_t phq = playerHQ();

  if (playerXq >= sxq + swq || playerXq + PLAYER_W_Q <= sxq) return;
  if (playerYq >= syq + shq || playerYq + phq <= syq) return;

  int32_t overlapL = (playerXq + PLAYER_W_Q) - sxq;
  int32_t overlapR = (sxq + swq) - playerXq;
  int32_t overlapT = (playerYq + phq) - syq;
  int32_t overlapB = (syq + shq) - playerYq;

  int32_t minX = (overlapL < overlapR) ? overlapL : overlapR;
  int32_t minY = (overlapT < overlapB) ? overlapT : overlapB;

  if (minX < minY) {
    playerXq += (overlapL < overlapR) ? -overlapL : overlapR;
    velXq = 0;
  } else if (overlapT < overlapB) {
    playerYq -= overlapT;
    velYq = 0;
    onGround = true;
  } else {
    playerYq += overlapB;
    if (rising) {
      if (type == O_BLOCK) bustBrick(section, index);
      else if (type == O_QBLOCK) hitQBlock(section, index);
    }
    if (velYq < 0) velYq = 0;
  }
}

// canBust: only the post-Y-move pass may break bricks (head hit while
// rising). Side scrapes from the X pass must not.
static void resolveSolids(bool canBust) {
  int16_t playerPx = (int16_t)(playerXq >> 8);
  bool rising = canBust && velYq < 0;

  for (uint8_t i = 0; i < worldCount; i++) {
    uint8_t type = pgm_read_byte(&worldObjs[i].type);
    if (!objSolid(type)) continue;
    if (type == O_BLOCK && isBroken(0, i)) continue;

    int32_t sx = (int16_t)pgm_read_word(&worldObjs[i].x);
    int16_t sw = objWidth(type);
    int32_t gap = sx - playerPx;
    if (gap > PLAYER_W || gap < -sw) continue;

    resolveBox(sx, pgm_read_byte(&worldObjs[i].y), sw, objHeight(type),
               rising, 0, i, type);
  }

  for (uint8_t b = 0; b < beamCount; b++) {
    int32_t sx = beams[b].x;
    int16_t sw = beams[b].w;
    int32_t gap = sx - playerPx;
    if (gap > PLAYER_W || gap < -sw) continue;
    resolveBox(sx, beams[b].y, sw, BEAM_H, rising, 0, 0, O_PLAT);
  }
}

void updatePlayer(const Buttons& btn) {
  velXq = 0;
  if (btn.left) {
    velXq = -MOVE_SPEED_Q;
    facingRight = false;
  } else if (btn.right) {
    velXq = MOVE_SPEED_Q;
    facingRight = true;
  }
  if (btn.b && velXq) velXq += velXq >> 1;  // B: 1.5x run

  // A jumps; hold length = height (extra gravity while rising if released).
  // B at takeoff adds launch speed so run-jumps clear the high bricks.
  // Water: A hold swims up; no ground required.
  bool jumpHeld = btn.a;
  if (levelFlags & LF_SWIM) {
    velYq += SWIM_GRAVITY_Q;
    if (jumpHeld) velYq = SWIM_UP_Q;
    if (velYq > SWIM_MAX_FALL_Q) velYq = SWIM_MAX_FALL_Q;
    if (jumpHeld && !jumpWasHeld) audioPlay(SFX_JUMP);
  } else {
    if (jumpHeld && !jumpWasHeld && onGround) {
      velYq = JUMP_VEL_Q;
      if (btn.b) velYq -= VEL_Q(30);  // ~60 px run-jump clears bricks
      onGround = false;
      audioPlay(SFX_JUMP);
    }
    velYq += GRAVITY_Q;
    if (!jumpHeld && velYq < 0) velYq += GRAVITY_Q;
    if (velYq > MAX_FALL_Q) velYq = MAX_FALL_Q;
  }
  jumpWasHeld = jumpHeld;

  playerXq += velXq;
  if (playerXq < 0) {
    playerXq = 0;
    velXq = 0;
  }
  {
    int32_t maxXq = (int32_t)(levelW - PLAYER_W) << 8;
    if (playerXq > maxXq) {
      playerXq = maxXq;
      velXq = 0;
    }
  }
  resolveSolids(false);

  playerYq += velYq;
  onGround = false;
  resolveSolids(true);

  if (levelFlags & LF_PITS) {
    if ((int16_t)(playerYq >> 8) > SCREEN_HEIGHT) {
      forceDeath();
      return;
    }
  } else {
    int32_t floorQ = (int32_t)(GROUND_Y - playerH()) << 8;
    if (playerYq >= floorQ) {
      playerYq = floorQ;
      velYq = 0;
      onGround = true;
    }
  }
  if ((levelFlags & LF_LAVA) &&
      (int16_t)(playerYq >> 8) + playerH() >= LAVA_Y) {
    forceDeath();
    return;
  }

  int16_t playerPx = (int16_t)(playerXq >> 8);
  cameraX = playerPx - CAMERA_MARGIN;
  if (cameraX < 0) cameraX = 0;
  {
    int32_t maxCam = (int32_t)levelW - SCREEN_WIDTH;
    if (maxCam < 0) maxCam = 0;
    if (cameraX > maxCam) cameraX = maxCam;
  }

  animFrame = (uint8_t)((playerPx >> 3) & 1);
}

int16_t enemyWidth(const Enemy& e) {
  if (e.type == E_BOWSER) return BOWSER_W;
  if (e.type == E_FISH) return FISH_W;
  return GOOMBA_W;
}

int16_t enemyHeight(const Enemy& e) {
  if (e.state == ES_SQUASH) return SQUASH_H;
  if (e.type == E_BOWSER) return BOWSER_H;
  if (e.type == E_FISH) return FISH_H;
  return GOOMBA_H;
}

int16_t enemyMaxHeight(const Enemy& e) {
  if (e.type == E_BOWSER) return BOWSER_H;
  if (e.type == E_FISH) return FISH_H;
  return GOOMBA_H;
}

void resetLevel() {
  loadLevel(levelIdx);
  playerXq = (int32_t)16 << 8;
  playerYq = (int32_t)(levelSpawnY - playerH()) << 8;
  velXq = 0;
  velYq = 0;
  onGround = true;
  facingRight = true;
  jumpWasHeld = false;
  cameraX = 0;
  playState = PLAY_RUN;
  deathNeedsRender = false;

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) enemies[i].type = E_NONE;
  spawnFrontier = 0;
  brokenCount = 0;
  usedQCount = 0;
  pendingEraseCount = 0;
  panelValid = false;
}

// Fatal hit: freeze, then death hop. Ignores power-up state.
static void forceDeath() {
  if (playState != PLAY_RUN) return;
  playState = PLAY_DEATH_HOLD;
  deathMs = millis();
  deathNeedsRender = true;
  velXq = 0;
  velYq = 0;
  audioStopBgm();
  audioPlay(SFX_DEATH);
}

// Big Mario shrinks with brief invulnerability; small Mario dies.
static void playerHit() {
  forceDeath();
}

static void endDeath() {
  playState = PLAY_RUN;
  deathNeedsRender = false;
  if (lives > 1) {
    lives--;
    enterMode(MODE_LIVES);
  } else {
    lives = 0;
    audioPlay(SFX_GAMEOVER);
    enterMode(MODE_DEAD);
  }
}

// Death hop only: gravity, no solids, no ground, camera locked.
// Terminal velocity is left uncapped so the fall clears the panel quickly.
void updateDeathFall() {
  velYq += GRAVITY_Q;
  playerYq += velYq;

  if ((playerYq >> 8) > SCREEN_HEIGHT) {
    endDeath();
  }
}

void spawnEnemies() {
  int32_t limit = cameraX + SCREEN_WIDTH + 8;
  if (limit > levelW) limit = levelW;
  if (limit <= spawnFrontier) return;

  for (uint8_t d = 0; d < spawnCount; d++) {
    int32_t ex = (int16_t)pgm_read_word(&worldSpawns[d].x);
    if (ex < spawnFrontier || ex >= limit) continue;

    for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
      Enemy& e = enemies[i];
      if (e.type != E_NONE) continue;

      e.type = pgm_read_byte(&worldSpawns[d].type);
      e.state = ES_WALK;
      e.timer = (e.type == E_BOWSER) ? BOWSER_HP : 0;
      if (e.type == E_FISH) e.vxq = -FISH_SPEED_Q;
      else if (e.type == E_BOWSER) e.vxq = -BOWSER_SPEED_Q;
      else e.vxq = -GOOMBA_SPEED_Q;
      e.vyq = 0;
      e.xq = ex << 8;
      uint8_t sy = pgm_read_byte(&worldSpawns[d].y);
      int16_t top = (e.type == E_FISH) ? sy
                    : ((sy ? sy : GROUND_Y) - enemyHeight(e));
      e.yq = (int32_t)top << 8;
      e.prevX = ex;
      e.prevY = top;
      break;
    }
  }

  spawnFrontier = limit;
}

// Shared walk / gravity / floor for enemies and mushrooms.
static void integrateActor(int32_t& xq, int32_t& yq, int16_t& vxq, int16_t& vyq,
                    int16_t w, int16_t h) {
  int32_t wq = (int32_t)w << 8;
  int32_t hq = (int32_t)h << 8;

  if (vxq) {
    xq += vxq;
    if (boxVsSolids(xq, yq, wq, hq, NULL)) {
      xq -= vxq;
      vxq = -vxq;
    }
  }

  vyq += GRAVITY_Q;
  if (vyq > MAX_FALL_Q) vyq = MAX_FALL_Q;
  yq += vyq;

  int32_t lift;
  if (boxVsSolids(xq, yq, wq, hq, &lift)) {
    yq -= lift;
    if (vyq > 0) vyq = 0;
  }

  int32_t floorQ = (int32_t)(GROUND_Y - h) << 8;
  if (!(levelFlags & LF_PITS) && yq >= floorQ) {
    yq = floorQ;
    vyq = 0;
  } else if ((yq >> 8) > SCREEN_HEIGHT + 16) {
    yq = (int32_t)(SCREEN_HEIGHT + 16) << 8;
    vxq = 0;
    vyq = 0;
  }
}

static void updateEnemy(Enemy& e) {
  if (e.state == ES_SQUASH) {
    if (--e.timer == 0) e.state = ES_GONE;
    return;
  }
  if (e.type == E_FISH) {
    e.xq += e.vxq;
    return;
  }
  integrateActor(e.xq, e.yq, e.vxq, e.vyq, enemyWidth(e), enemyHeight(e));
}

void updateEnemies() {
  int32_t camLeft = cameraX;

  if (++animTick >= ANIM_TICKS) {
    animTick = 0;
    enemyFrame ^= 1;
  }

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE || e.state == ES_GONE) continue;

    updateEnemy(e);

    int32_t ex = e.xq >> 8;
    if (ex + enemyWidth(e) < camLeft - 8 || ex > camLeft + SCREEN_WIDTH + 96 ||
        (int16_t)(e.yq >> 8) > SCREEN_HEIGHT) {
      e.state = ES_GONE;
    }
  }
}

// Contact kill from a kicked shell - Goombas flatten, everything else
// drops out via ES_GONE (same lethality as touching the player).
static void defeatEnemy(Enemy& e) {
  if (e.state == ES_GONE || e.state == ES_SQUASH) return;

  if (e.type == E_BOWSER) {
    if (e.timer) e.timer--;
    if (e.timer == 0) e.state = ES_GONE;
    e.vxq = 0;
    audioPlay(SFX_STOMP);
    return;
  }
  if (e.type == E_GOOMBA) {
    e.state = ES_SQUASH;
    e.timer = SQUASH_TICKS;
    e.vxq = 0;
    e.yq += (int32_t)(GOOMBA_H - SQUASH_H) << 8;
  } else {
    e.state = ES_GONE;
    e.vxq = 0;
  }
  audioPlay(SFX_STOMP);
}

void collideEnemies() {
  int32_t phq = playerHQ();

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE || e.state == ES_GONE || e.state == ES_SQUASH) continue;

    int32_t wq = (int32_t)enemyWidth(e) << 8;
    int32_t hq = (int32_t)enemyHeight(e) << 8;

    if (playerXq >= e.xq + wq || playerXq + PLAYER_W_Q <= e.xq) continue;
    if (playerYq >= e.yq + hq || playerYq + phq <= e.yq) continue;

    if (e.type == E_FISH) {
      playerHit();
      return;
    }

    if (!onGround) {
      playerYq = e.yq - phq;
      velYq = STOMP_VEL_Q;
      onGround = false;
      defeatEnemy(e);
    } else {
      playerHit();
      return;
    }
  }
}

void collideCoins() {
  int16_t playerPx = (int16_t)(playerXq >> 8);
  int32_t phq = playerHQ();
  int32_t cwq = (int32_t)objWidth(O_COIN) << 8;
  int32_t chq = (int32_t)objHeight(O_COIN) << 8;

  for (uint8_t i = 0; i < worldCount; i++) {
    if (pgm_read_byte(&worldObjs[i].type) != O_COIN) continue;
    if (isBroken(0, i)) continue;

    int32_t sx = (int16_t)pgm_read_word(&worldObjs[i].x);
    int32_t gap = sx - playerPx;
    if (gap > PLAYER_W || gap < -(int16_t)objWidth(O_COIN)) continue;

    int32_t sxq = sx << 8;
    int32_t syq = (int32_t)pgm_read_byte(&worldObjs[i].y) << 8;
    if (playerXq >= sxq + cwq || playerXq + PLAYER_W_Q <= sxq) continue;
    if (playerYq >= syq + chq || playerYq + phq <= syq) continue;

    collectCoin(0, i);
  }
}

void collideFlag() {
  if (playState != PLAY_RUN) return;

  int16_t playerPx = (int16_t)(playerXq >> 8);
  uint8_t poleW = (levelTheme == TH_CASTLE) ? 10 : 3;
  if (playerPx + PLAYER_W <= flagX || playerPx >= flagX + poleW) return;

  int16_t py = (int16_t)(playerYq >> 8);
  if (py + playerH() <= flagTop || py >= (int16_t)flagTop + 72) return;

  audioStopBgm();
  audioPlay(SFX_POWERUP);
  if (levelIdx + 1 < LEVEL_COUNT) {
    levelIdx++;
    enterMode(MODE_LIVES);
  } else {
    enterMode(MODE_WIN);
  }
}

void updateBeams() {
  int16_t px = (int16_t)(playerXq >> 8);
  int16_t py = (int16_t)(playerYq >> 8);
  int16_t feet = py + playerH();

  for (uint8_t i = 0; i < beamCount; i++) {
    Beam& b = beams[i];
    int16_t oldX = b.x;
    b.x = (int16_t)(b.x + b.vx);
    if (b.x <= b.xMin || b.x >= b.xMax) {
      b.vx = (int8_t)-b.vx;
      b.x = (int16_t)(b.x + b.vx);
    }
    if (onGround && feet >= b.y && feet <= (int16_t)(b.y + BEAM_H + 2)) {
      if (px + PLAYER_W > oldX && px < oldX + b.w) {
        playerXq += (int32_t)(b.x - oldX) << 8;
      }
    }
  }
}
