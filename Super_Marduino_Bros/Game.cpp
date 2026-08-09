#include "Game.h"
#include "World.h"
#include "UI.h"
#include "Audio.h"
#include <avr/pgmspace.h>

// Owned by Display.cpp; cleared when a level boots so GRAM refills.
extern bool panelValid;

Enemy enemies[MAX_ENEMIES];
Item items[MAX_ITEMS];

uint16_t score = 0;
uint16_t timeLeft = START_TIME;
uint8_t timeTick = 0;

int32_t spawnFrontier = 0;
uint8_t animTick = 0;
uint8_t enemyFrame = 0;

bool bigMario = false;
uint8_t invulnTicks = 0;

int32_t playerXq = (int32_t)40 << 8;
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

static void spawnMushroom(int32_t worldX, int16_t blockY) {
  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type != IT_NONE) continue;

    it.type = IT_MUSHROOM;
    it.state = IS_RISE;
    it.xq = worldX << 8;
    it.yq = (int32_t)blockY << 8;
    it.riseTargetY = (int16_t)(blockY - MUSH_H);
    it.vxq = MUSH_SPEED_Q;
    it.vyq = 0;
    it.prevX = worldX;
    it.prevY = blockY;
    return;
  }
}

// Empty the ? block (stays solid) and release its loot once.
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
  audioPlay(SFX_BUMP);

  uint8_t loot = pgm_read_byte(&WORLD_LOOT[index]);
  if (loot == Q_MUSHROOM) {
    int32_t wx = section * SECTION_W + (int16_t)pgm_read_word(&WORLD[index].x);
    int16_t by = pgm_read_byte(&WORLD[index].y);
    spawnMushroom(wx, by);
  }
}

static void growMario() {
  if (bigMario) return;
  bigMario = true;
  // Keep feet planted while the hitbox grows upward.
  playerYq -= (int32_t)(PLAYER_H_BIG - PLAYER_H_SMALL) << 8;
  audioPlay(SFX_POWERUP);
}

static bool boxVsSolids(int32_t xq, int32_t yq, int32_t wq, int32_t hq, int32_t* liftOut) {
  int16_t px = (int16_t)(xq >> 8);
  int32_t baseSection = ((int32_t)px / SECTION_W) - 1;
  if (px < 0) baseSection--;

  bool hit = false;
  int32_t lift = 0;

  for (int8_t s = 0; s < 3; s++) {
    int32_t section = baseSection + s;
    int32_t origin = section * SECTION_W;

    for (uint8_t i = 0; i < WORLD_COUNT; i++) {
      uint8_t type = pgm_read_byte(&WORLD[i].type);
      if (!objSolid(type)) continue;
      if (type == O_BLOCK && isBroken(section, i)) continue;

      int32_t sx = origin + (int16_t)pgm_read_word(&WORLD[i].x);
      int16_t sw = objWidth(type);

      int32_t gap = sx - px;
      if (gap > (wq >> 8) || gap < -sw) continue;

      int32_t sxq = sx << 8;
      int32_t syq = (int32_t)pgm_read_byte(&WORLD[i].y) << 8;
      int32_t swq = (int32_t)sw << 8;
      int32_t shq = (int32_t)objHeight(type) << 8;

      if (xq >= sxq + swq || xq + wq <= sxq) continue;
      if (yq >= syq + shq || yq + hq <= syq) continue;

      hit = true;
      int32_t up = (yq + hq) - syq;
      if (up > lift) lift = up;
    }
  }

  if (liftOut) *liftOut = lift;
  return hit;
}

// canBust: only the post-Y-move pass may break bricks (head hit while
// rising). Side scrapes from the X pass must not.
static void resolveSolids(bool canBust) {
  int16_t playerPx = (int16_t)(playerXq >> 8);
  int32_t baseSection = ((int32_t)playerPx / SECTION_W) - 1;
  if (playerPx < 0) baseSection--;

  // Latch once so a wide head can bust every brick it hits this step,
  // even after the first ceiling resolution zeroes velYq.
  bool rising = canBust && velYq < 0;

  for (int8_t s = 0; s < 3; s++) {
    int32_t section = baseSection + s;
    int32_t origin = section * SECTION_W;

    for (uint8_t i = 0; i < WORLD_COUNT; i++) {
      uint8_t type = pgm_read_byte(&WORLD[i].type);
      if (!objSolid(type)) continue;
      if (type == O_BLOCK && isBroken(section, i)) continue;

      int32_t sx = origin + (int16_t)pgm_read_word(&WORLD[i].x);
      int16_t sw = objWidth(type);

      int32_t gap = sx - playerPx;
      if (gap > PLAYER_W || gap < -sw) continue;

      int32_t sxq = sx << 8;
      int32_t syq = (int32_t)pgm_read_byte(&WORLD[i].y) << 8;
      int32_t swq = (int32_t)sw << 8;
      int32_t shq = (int32_t)objHeight(type) << 8;
      int32_t phq = playerHQ();

      if (playerXq >= sxq + swq || playerXq + PLAYER_W_Q <= sxq) continue;
      if (playerYq >= syq + shq || playerYq + phq <= syq) continue;

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
          if (type == O_BLOCK) bustBrick(section, i);
          else if (type == O_QBLOCK) hitQBlock(section, i);
        }
        if (velYq < 0) velYq = 0;
      }
    }
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
  bool jumpHeld = btn.a;
  if (jumpHeld && !jumpWasHeld && onGround) {
    velYq = JUMP_VEL_Q;
    if (btn.b) velYq -= VEL_Q(30);  // ~60 px run-jump clears bricks
    onGround = false;
    audioPlay(SFX_JUMP);
  }
  jumpWasHeld = jumpHeld;

  velYq += GRAVITY_Q;
  if (!jumpHeld && velYq < 0) velYq += GRAVITY_Q;
  if (velYq > MAX_FALL_Q) velYq = MAX_FALL_Q;

  playerXq += velXq;
  if (playerXq < 0) {
    playerXq = 0;
    velXq = 0;
  }
  resolveSolids(false);

  playerYq += velYq;
  onGround = false;
  resolveSolids(true);

  int32_t floorQ = (int32_t)(GROUND_Y - playerH()) << 8;
  if (playerYq >= floorQ) {
    playerYq = floorQ;
    velYq = 0;
    onGround = true;
  }

  if (invulnTicks) invulnTicks--;

  int16_t playerPx = (int16_t)(playerXq >> 8);
  cameraX = playerPx - CAMERA_MARGIN;
  if (cameraX < 0) cameraX = 0;

  animFrame = (uint8_t)((playerPx >> 3) & 1);
}

int16_t enemyWidth(const Enemy& e) {
  return (e.type == E_KOOPA) ? KOOPA_W : GOOMBA_W;
}

int16_t enemyHeight(const Enemy& e) {
  if (e.state == ES_SQUASH) return SQUASH_H;
  if (e.type == E_KOOPA) return (e.state == ES_WALK) ? KOOPA_H : SHELL_H;
  return GOOMBA_H;
}

// Tallest pose a slot can take, so a dirty rect stays valid across a
// Koopa collapsing into its shell.
int16_t enemyMaxHeight(const Enemy& e) {
  return (e.type == E_KOOPA) ? KOOPA_H : GOOMBA_H;
}

void resetLevel() {
  bigMario = false;
  invulnTicks = 0;
  playerXq = (int32_t)40 << 8;
  playerYq = (int32_t)(GROUND_Y - PLAYER_H_SMALL) << 8;
  velXq = 0;
  velYq = 0;
  onGround = true;
  facingRight = true;
  jumpWasHeld = false;
  cameraX = 0;
  playState = PLAY_RUN;
  deathNeedsRender = false;
  timeLeft = START_TIME;
  timeTick = 0;

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) enemies[i].type = E_NONE;
  for (uint8_t i = 0; i < MAX_ITEMS; i++) items[i].type = IT_NONE;
  spawnFrontier = 0;
  brokenCount = 0;
  usedQCount = 0;
  pendingEraseCount = 0;
  panelValid = false;
}

// Fatal hit / time-up: freeze, then death hop. Ignores power-up state.
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
  if (playState != PLAY_RUN) return;
  if (bigMario) {
    bigMario = false;
    playerYq += (int32_t)(PLAYER_H_BIG - PLAYER_H_SMALL) << 8;
    invulnTicks = INVULN_TICKS;
    audioPlay(SFX_POWERDOWN);
    return;
  }
  forceDeath();
}

void tickTimer() {
  if (++timeTick < TIME_TICKS) return;
  timeTick = 0;
  if (timeLeft == 0) return;
  timeLeft--;
  if (timeLeft == 0) forceDeath();
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
  if (limit <= spawnFrontier) return;

  for (int32_t s = spawnFrontier / SECTION_W; s <= limit / SECTION_W; s++) {
    int32_t origin = s * SECTION_W;

    for (uint8_t d = 0; d < SPAWN_COUNT; d++) {
      int32_t ex = origin + (int16_t)pgm_read_word(&SPAWNS[d].x);
      if (ex < spawnFrontier || ex >= limit) continue;

      for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
        Enemy& e = enemies[i];
        if (e.type != E_NONE) continue;

        e.type = pgm_read_byte(&SPAWNS[d].type);
        e.state = ES_WALK;
        e.timer = 0;
        e.vxq = (e.type == E_KOOPA) ? -KOOPA_SPEED_Q : -GOOMBA_SPEED_Q;
        e.vyq = 0;
        e.xq = ex << 8;
        e.yq = (int32_t)(GROUND_Y - enemyHeight(e)) << 8;
        e.prevX = ex;
        e.prevY = (int16_t)(e.yq >> 8);
        break;
      }
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
  if (yq >= floorQ) {
    yq = floorQ;
    vyq = 0;
  }
}

static void updateEnemy(Enemy& e) {
  if (e.state == ES_SQUASH) {
    if (--e.timer == 0) e.state = ES_GONE;
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
    if (ex + enemyWidth(e) < camLeft - 8 || ex > camLeft + SCREEN_WIDTH + 96) {
      e.state = ES_GONE;
    }
  }
}

// Contact kill from a kicked shell - Goombas flatten, everything else
// drops out via ES_GONE (same lethality as touching the player).
static void defeatEnemy(Enemy& e) {
  if (e.state == ES_GONE || e.state == ES_SQUASH) return;

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

// Sliding shells wipe out anything they overlap. O(n^2) over MAX_ENEMIES
// (6) so the cost is a handful of box tests per step.
void collideShellHits() {
  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& shell = enemies[i];
    if (shell.type == E_NONE || shell.state != ES_SLIDE) continue;

    int32_t swq = (int32_t)enemyWidth(shell) << 8;
    int32_t shq = (int32_t)enemyHeight(shell) << 8;

    for (uint8_t j = 0; j < MAX_ENEMIES; j++) {
      if (i == j) continue;
      Enemy& e = enemies[j];
      if (e.type == E_NONE || e.state == ES_GONE || e.state == ES_SQUASH) continue;

      int32_t ewq = (int32_t)enemyWidth(e) << 8;
      int32_t ehq = (int32_t)enemyHeight(e) << 8;

      if (shell.xq >= e.xq + ewq || shell.xq + swq <= e.xq) continue;
      if (shell.yq >= e.yq + ehq || shell.yq + shq <= e.yq) continue;

      defeatEnemy(e);
    }
  }
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

    // Airborne contact is always a stomp - forgiving when jumping on heads.
    // Ground contact hurts, except a parked shell which gets kicked.
    if (!onGround) {
      playerYq = e.yq - phq;
      velYq = STOMP_VEL_Q;
      onGround = false;

      if (e.type == E_GOOMBA) {
        e.state = ES_SQUASH;
        e.timer = SQUASH_TICKS;
        e.vxq = 0;
        e.yq += (int32_t)(GOOMBA_H - SQUASH_H) << 8;
        audioPlay(SFX_STOMP);
      } else if (e.state == ES_WALK) {
        e.state = ES_SHELL;
        e.vxq = 0;
        e.yq += (int32_t)(KOOPA_H - SHELL_H) << 8;
        audioPlay(SFX_STOMP);
      } else if (e.state == ES_SLIDE) {
        e.state = ES_SHELL;
        e.vxq = 0;
        audioPlay(SFX_STOMP);
      } else {
        e.state = ES_SLIDE;
        e.vxq = facingRight ? SHELL_SPEED_Q : -SHELL_SPEED_Q;
        audioPlay(SFX_KICK);
      }
    } else if (e.state == ES_SHELL) {
      bool fromLeft = playerXq < e.xq;
      e.state = ES_SLIDE;
      e.vxq = fromLeft ? SHELL_SPEED_Q : -SHELL_SPEED_Q;
      playerXq += fromLeft ? -(int32_t)(3 << 8) : (int32_t)(3 << 8);
      audioPlay(SFX_KICK);
    } else if (invulnTicks == 0) {
      playerHit();
      return;
    }
  }
}

static void updateItem(Item& it) {
  if (it.state == IS_RISE) {
    it.yq += MUSH_RISE_Q;
    if ((int16_t)(it.yq >> 8) <= it.riseTargetY) {
      it.yq = (int32_t)it.riseTargetY << 8;
      it.state = IS_WALK;
    }
    return;
  }
  integrateActor(it.xq, it.yq, it.vxq, it.vyq, MUSH_W, MUSH_H);
}

void updateItems() {
  int32_t camLeft = cameraX;

  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE || it.state == IS_GONE) continue;

    updateItem(it);

    int32_t ix = it.xq >> 8;
    if (ix + MUSH_W < camLeft - 8 || ix > camLeft + SCREEN_WIDTH + 96) {
      it.state = IS_GONE;
    }
  }
}

void collideItems() {
  int32_t phq = playerHQ();
  int32_t mwq = (int32_t)MUSH_W << 8;
  int32_t mhq = (int32_t)MUSH_H << 8;

  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE || it.state == IS_GONE) continue;

    if (playerXq >= it.xq + mwq || playerXq + PLAYER_W_Q <= it.xq) continue;
    if (playerYq >= it.yq + mhq || playerYq + phq <= it.yq) continue;

    growMario();
    it.state = IS_GONE;
  }
}

void collideCoins() {
  int16_t playerPx = (int16_t)(playerXq >> 8);
  int32_t baseSection = ((int32_t)playerPx / SECTION_W) - 1;
  if (playerPx < 0) baseSection--;
  int32_t phq = playerHQ();
  int32_t cwq = (int32_t)objWidth(O_COIN) << 8;
  int32_t chq = (int32_t)objHeight(O_COIN) << 8;

  for (int8_t s = 0; s < 3; s++) {
    int32_t section = baseSection + s;
    int32_t origin = section * SECTION_W;

    for (uint8_t i = 0; i < WORLD_COUNT; i++) {
      if (pgm_read_byte(&WORLD[i].type) != O_COIN) continue;
      if (isBroken(section, i)) continue;

      int32_t sx = origin + (int16_t)pgm_read_word(&WORLD[i].x);
      int32_t gap = sx - playerPx;
      if (gap > PLAYER_W || gap < -(int16_t)objWidth(O_COIN)) continue;

      int32_t sxq = sx << 8;
      int32_t syq = (int32_t)pgm_read_byte(&WORLD[i].y) << 8;
      if (playerXq >= sxq + cwq || playerXq + PLAYER_W_Q <= sxq) continue;
      if (playerYq >= syq + chq || playerYq + phq <= syq) continue;

      collectCoin(section, i);
    }
  }
}
