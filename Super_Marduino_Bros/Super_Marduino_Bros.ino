/***************************************************
  Mario-style demo - SSD1351 128x128 OLED on Arduino Nano

  Controls: NES Classic / clone controller over I2C
    VCC -> 3.3V, GND -> GND, SDA -> A4, SCL -> A5

  Audio: passive piezo/buzzer on D9 (Timer1 CTC), see Audio.h / Audio.cpp

  *** MOUNT THE DISPLAY ROTATED 90 DEGREES COUNTER-CLOCKWISE ***

  HARDWARE SCROLLING
  ------------------
  SPI on an ATmega328P is capped at F_CPU/2 = 8 MHz, so a pixel
  costs ~2 us and a full frame is ~33 ms of bus time. The only way
  past that is to stop sending the parts of the frame that did not
  change - and when the camera pans, a software renderer has to
  resend everything.

  The SSD1351 has a Set Display Start Line register (0xA1) that
  slides the panel over its own 128-row GRAM with wraparound, for
  the cost of one command. That axis is the panel's vertical one,
  so this renderer is transposed: one GRAM row holds one *column*
  of the game world, and GRAM behaves as a 128-entry ring buffer of
  world columns. Panning one pixel means writing the single column
  that just scrolled into view and bumping the start line. The
  panel is square, so rotating the module 90 degrees costs no
  screen area.

  Scrolling therefore went from ~16000 pixels a frame to ~600:
  one new world column plus a repaint of the ~15 columns Mario
  occupies. Every enemy costs another dirty rectangle of its own -
  about 240 pixels - which is why they are capped at MAX_ENEMIES
  and despawn as soon as they leave the panel.

  IF THE PICTURE COMES OUT WRONG, two one-line toggles cover every
  orientation - see FLIP_Y and SCROLL_REVERSE below.
 ****************************************************/

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 128

#define DC_PIN   7
#define CS_PIN  10
#define RST_PIN  8

// Sky ends up on the wrong side / picture mirrored? Flip this.
#define FLIP_Y 1

// World scrolls the wrong way as Mario runs? Flip this. Mounting the
// panel the other way round mirrors the axis the start line slides
// along, and an offset cannot undo a mirror, so this fills the GRAM
// ring backwards instead - see gramRow().
#define SCROLL_REVERSE 1

// Menus use Adafruit's default font via setRotation so text is upright
// on the panel (mounted 90 deg CCW). Gameplay uses applyGameRemap()
// instead. If title text is sideways, try 3 here.
#define UI_ROTATION 1

// Drop to 100000 if a long controller cable gets flaky.
#define I2C_CLOCK 400000

// Serial FPS / boot logs. Off by default — HardwareSerial alone is ~1 KB
// and the Nano is flash-bound with score/coins/run-jump in.
#ifndef DEBUG_SERIAL
#define DEBUG_SERIAL 0
#endif

#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>
#include <Wire.h>
#include "Audio.h"

Adafruit_SSD1351 tft = Adafruit_SSD1351(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, CS_PIN, DC_PIN, RST_PIN);

const uint8_t NES_I2C_ADDR = 0x52;

const uint16_t SKY_BLUE   = 0x4D7F;
const uint16_t HILL_GREEN = 0x2589;
const uint16_t HILL_LIGHT = 0x36CC;
const uint16_t GRASS      = 0x4E20;
const uint16_t DIRT       = 0xA285;
const uint16_t DARK_DIRT  = 0x6182;
const uint16_t ORANGE     = 0xFC60;
const uint16_t PIPE_HI    = 0x87E0;
const uint16_t SKIN       = 0xFD20;
const uint16_t GOOMBA_BR  = 0xAD04;
const uint16_t GOOMBA_FT  = 0xF64F;
const uint16_t KOOPA_SKIN = 0xFECF;
const uint16_t SHELL_GRN  = 0x0404;
const uint16_t SHELL_LT   = 0x770B;
const uint16_t SHELL_RIM  = 0xFF14;
const uint16_t LUIGI_GRN  = 0x07C0;
const uint16_t UI_DIM     = 0x8410;

const int16_t GROUND_Y = 104;
const int16_t SECTION_W = 320;
const int16_t PLAYER_W = 14;
const int16_t PLAYER_H_SMALL = 14;
const int16_t PLAYER_H_BIG   = 22;
const int16_t CAMERA_MARGIN = 40;

const uint8_t STEP_MS      = 33;    // 16ms @ 60 fps, 33ms @ 30 fps

// The integrator works in 8.8 pixels per step, which means every
// motion constant has to be rescaled whenever STEP_MS changes - a
// velocity with the step, an acceleration with its square. Authoring
// them in real-world units and converting at compile time keeps the
// run speed and the jump arc identical at any frame rate.
#define VEL_Q(px_s)  ((int16_t)(((int32_t)(px_s) * 256 * STEP_MS + 500) / 1000))
#define ACC_Q(px_s2) ((int16_t)(((int32_t)(px_s2) * 256 * STEP_MS * STEP_MS \
                                 + 500000L) / 1000000L))

const int16_t MOVE_SPEED_Q = VEL_Q(50);    // 50 px/s walk; B run is 1.5x
const int16_t GRAVITY_Q    = ACC_Q(214);   // 214 px/s^2
const int16_t JUMP_VEL_Q   = -VEL_Q(130);  // walk jump ~40 px; B adds VEL_Q(30)
const int16_t MAX_FALL_Q   = VEL_Q(150);   // terminal velocity
const int16_t STOMP_VEL_Q  = -VEL_Q(93);   // bounce off a stomped enemy
const int32_t PLAYER_W_Q   = (int32_t)PLAYER_W << 8;
const uint8_t INVULN_TICKS = (uint8_t)(2000 / STEP_MS);  // ~2 s after shrink

#define O_CLOUD  0
#define O_HILL   1
#define O_HILL2  2
#define O_BLOCK  3
#define O_QBLOCK 4
#define O_COIN   5
#define O_PIPE   6

struct ObjDef {
  int16_t x;
  uint8_t y;
  uint8_t type;
};

// One 320 px section of world, painted in table order so later
// entries cover earlier ones. Parallax is gone: hardware scrolling
// slides the whole GRAM at once, so the backdrop is now part of the
// world and repeats with it.
const ObjDef WORLD[] PROGMEM = {
  { 15, 17, O_CLOUD},
  {190, 31, O_CLOUD},
  {  5,  0, O_HILL},
  { 67,  0, O_HILL2},
  { 72, 59, O_BLOCK},
  { 87, 59, O_QBLOCK},
  {102, 59, O_BLOCK},
  {125, 48, O_COIN},
  {145, 70, O_COIN},
  {166, (uint8_t)(GROUND_Y - 34), O_PIPE},
  {200, 48, O_COIN},
  {230, 59, O_QBLOCK},
  {250, 30, O_COIN},
  {267, 48, O_BLOCK},
  {282, 48, O_BLOCK},
};
const uint8_t WORLD_COUNT = sizeof(WORLD) / sizeof(WORLD[0]);

// Loot for each WORLD entry. Only O_QBLOCK slots are read on head-hit;
// both question blocks hold a mushroom.
#define Q_NONE     0
#define Q_MUSHROOM 1
const uint8_t WORLD_LOOT[] PROGMEM = {
  Q_NONE, Q_NONE, Q_NONE, Q_NONE,
  Q_NONE, Q_MUSHROOM, Q_NONE, Q_NONE,
  Q_NONE, Q_NONE, Q_NONE, Q_MUSHROOM,
  Q_NONE, Q_NONE, Q_NONE,
};

// --- Enemies --------------------------------------------------------------

#define E_NONE   0
#define E_GOOMBA 1
#define E_KOOPA  2

#define ES_WALK   0
#define ES_SQUASH 1  // flattened Goomba, waits out a timer
#define ES_SHELL  2  // Koopa withdrawn and stationary
#define ES_SLIDE  3  // shell kicked and travelling
#define ES_GONE   4  // still on screen as stale pixels, erased next render

#define MAX_ENEMIES 6

const int16_t GOOMBA_W = 14;
const int16_t GOOMBA_H = 14;
const int16_t KOOPA_W  = 13;
const int16_t KOOPA_H  = 20;
const int16_t SHELL_H  = 12;
const int16_t SQUASH_H = 6;

const int16_t GOOMBA_SPEED_Q = VEL_Q(19);
const int16_t KOOPA_SPEED_Q  = VEL_Q(16);
const int16_t SHELL_SPEED_Q  = VEL_Q(75);
const uint8_t SQUASH_TICKS   = 640 / STEP_MS;  // ~0.64 s flattened
const uint8_t ANIM_TICKS     = 130 / STEP_MS;  // ~0.13 s per walk frame

struct EnemyDef {
  int16_t x;
  uint8_t type;
};

// The World 1-1 cast, laid out inside the same repeating 320 px
// section as the scenery: a lone Goomba before the pipe, a Koopa
// pacing between the pipe and the lone question block, and a pair
// of Goombas under the high blocks.
const EnemyDef SPAWNS[] PROGMEM = {
  {120, E_GOOMBA},
  {205, E_KOOPA},
  {250, E_GOOMBA},
  {272, E_GOOMBA},
};
const uint8_t SPAWN_COUNT = sizeof(SPAWNS) / sizeof(SPAWNS[0]);

struct Enemy {
  int32_t xq;
  int32_t yq;
  int32_t prevX;  // pixel position at the last repaint, for the dirty rect
  int16_t prevY;
  int16_t vxq;
  int16_t vyq;
  uint8_t type;
  uint8_t state;
  uint8_t timer;
};

Enemy enemies[MAX_ENEMIES];

// --- Power-ups (mushrooms from ? blocks) ----------------------------------

#define IT_NONE     0
#define IT_MUSHROOM 1

#define IS_RISE 0  // emerging from a just-hit ? block
#define IS_WALK 1
#define IS_GONE 2  // stale pixels; freed after dirty-rect erase

#define MAX_ITEMS 2

const int16_t MUSH_W = 14;
const int16_t MUSH_H = 14;
const int16_t MUSH_SPEED_Q = VEL_Q(19);
const int16_t MUSH_RISE_Q  = -VEL_Q(22);

struct Item {
  int32_t xq;
  int32_t yq;
  int32_t prevX;
  int16_t prevY;
  int16_t vxq;
  int16_t vyq;
  int16_t riseTargetY;  // pixel Y where the rise finishes (fully above block)
  uint8_t type;
  uint8_t state;
};

Item items[MAX_ITEMS];

// World objects removed at runtime (bust bricks, collected coins).
// WORLD stays in PROGMEM; these entries make collision/compose skip
// a gone instance. The GRAM erase is one-shot (see pendingErase).
#define MAX_BROKEN 12
struct BrokenBrick {
  int16_t section;
  uint8_t index;
};
BrokenBrick brokenBricks[MAX_BROKEN];
uint8_t brokenCount = 0;

// Score / timer HUD: 3x5 font drawn at 2x (6x10) in the sky strip.
#define COIN_POINTS   100
#define START_TIME    300
#define TIME_TICKS    (1000 / STEP_MS)  // ~1 s per countdown step
#define HUD_DIGIT_W   7                 // 6 px glyph + 1 gap
#define HUD_Y0        1
#define HUD_Y1        10
#define SCORE_HUD_X   2
#define SCORE_HUD_W   (5 * HUD_DIGIT_W)
#define TIME_HUD_W    (3 * HUD_DIGIT_W)
#define TIME_HUD_X    (SCREEN_WIDTH - 2 - TIME_HUD_W)
uint16_t score = 0;
uint16_t timeLeft = START_TIME;
uint8_t scoreDigits[5];
uint8_t timeDigits[3];
uint8_t timeTick = 0;

// ? blocks that have been hit: stay solid, paint as empty brick.
#define MAX_USED_Q 8
typedef BrokenBrick UsedQBlock;
UsedQBlock usedQBlocks[MAX_USED_Q];
uint8_t usedQCount = 0;

// Queued block rects to rewrite into GRAM once, world-first, so the
// brick disappears and whatever was behind it (sky, hill, etc.) shows.
#define MAX_ERASE 4
struct EraseRect {
  int32_t x;
  int16_t y;
  uint8_t w;
  uint8_t h;
};
EraseRect pendingErase[MAX_ERASE];
uint8_t pendingEraseCount = 0;

// World x already scanned for spawns. It only ever moves right, so
// walking back over old ground does not repopulate it.
int32_t spawnFrontier = 0;
uint8_t animTick = 0;
uint8_t enemyFrame = 0;

struct Buttons {
  bool up, down, left, right;
  bool a, b, select, start;
};

bool bigMario = false;
uint8_t invulnTicks = 0;

inline int16_t playerH() {
  return bigMario ? PLAYER_H_BIG : PLAYER_H_SMALL;
}

inline int32_t playerHQ() {
  return (int32_t)playerH() << 8;
}

int32_t playerXq = (int32_t)40 << 8;
int32_t playerYq = (int32_t)(GROUND_Y - PLAYER_H_SMALL) << 8;
int16_t velXq = 0;
int16_t velYq = 0;
bool onGround = true;
bool facingRight = true;
bool jumpWasHeld = false;
uint8_t animFrame = 0;
int32_t cameraX = 0;

// One world column: 128 pixels running from sky down to dirt.
uint16_t colBuf[SCREEN_HEIGHT];

// composeColumn only touches rows inside this band, so a repaint
// that only needs Mario's slice does not pay for a full column.
int16_t clipTop = 0;
int16_t clipBot = SCREEN_HEIGHT - 1;

int32_t panelCam = 0;
bool panelValid = false;
int32_t marioColPrev = 0;
int16_t marioRowPrev = 0;

uint8_t circTab[12][12];
bool controllerOk = false;

uint32_t lastStep = 0;
#if DEBUG_SERIAL
uint32_t lastReport = 0;
uint16_t frameCount = 0;
uint32_t pixelCount = 0;
#endif

// --- Game flow ------------------------------------------------------------

#define START_LIVES    3
#define LIVES_HOLD_MS  2000

// Death beat: freeze the fatal frame, then a normal jump and fall off-screen.
#define DEATH_HOLD_MS  450

enum : uint8_t {
  MODE_TITLE = 0,
  MODE_SELECT,
  MODE_LIVES,
  MODE_PLAY,
  MODE_DEAD
};

// Substates of MODE_PLAY. Pause/death stay on the game remap so we never
// pay for a full-screen UI clear mid-level.
enum : uint8_t {
  PLAY_RUN = 0,
  PLAY_PAUSE,
  PLAY_DEATH_HOLD,   // frozen world - show the mistake
  PLAY_DEATH_FALL    // no-collision jump, then fall through the map
};

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

void enterMode(uint8_t mode);
void endDeath();

// --- Controller -----------------------------------------------------------

bool initController() {
  Wire.begin();
  Wire.setClock(I2C_CLOCK);

  Wire.beginTransmission(NES_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xF0);
  Wire.write(0x55);
  Wire.endTransmission();
  delay(10);

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xFB);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(10);

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xFE);
  Wire.write(0x03);
  Wire.endTransmission();
  delay(10);

  return true;
}

Buttons readController() {
  Buttons btn = {};
  if (!controllerOk) {
    return btn;
  }

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    return btn;
  }

  uint8_t data[8];
  uint8_t got = Wire.requestFrom(NES_I2C_ADDR, (uint8_t)8);
  if (got < 6) {
    while (Wire.available()) Wire.read();
    return btn;
  }
  for (uint8_t i = 0; i < got && i < 8; i++) {
    data[i] = Wire.read();
  }
  while (Wire.available()) Wire.read();

  uint8_t b0 = (got >= 8) ? data[6] : data[4];
  uint8_t b1 = (got >= 8) ? data[7] : data[5];

  btn.right  = !(b0 & 0x80);
  btn.down   = !(b0 & 0x40);
  btn.select = !(b0 & 0x10);
  btn.start  = !(b0 & 0x04);
  btn.b      = !(b1 & 0x40);
  btn.a      = !(b1 & 0x10);
  btn.left   = !(b1 & 0x02);
  btn.up     = !(b1 & 0x01);

  return btn;
}

// --- World geometry -------------------------------------------------------

const uint8_t OBJ_W[] PROGMEM = {28, 51, 51, 15, 15, 9, 24};
const uint8_t OBJ_H[] PROGMEM = {15, 15, 15, 15, 15, 15, 34};

uint8_t objWidth(uint8_t t)  { return pgm_read_byte(&OBJ_W[t]); }
uint8_t objHeight(uint8_t t) { return pgm_read_byte(&OBJ_H[t]); }

bool objSolid(uint8_t t) {
  return t == O_BLOCK || t == O_QBLOCK || t == O_PIPE;
}

bool slotMarked(const BrokenBrick* list, uint8_t count,
                int32_t section, uint8_t index) {
  for (uint8_t i = 0; i < count; i++) {
    if (list[i].section == (int16_t)section && list[i].index == index) {
      return true;
    }
  }
  return false;
}

bool isBroken(int32_t section, uint8_t index) {
  return slotMarked(brokenBricks, brokenCount, section, index);
}

bool isUsedQ(int32_t section, uint8_t index) {
  return slotMarked(usedQBlocks, usedQCount, section, index);
}

void fillDigits(uint8_t* out, uint8_t n, uint16_t v) {
  for (int8_t i = (int8_t)n - 1; i >= 0; i--) {
    out[i] = v % 10;
    v /= 10;
  }
}

void refreshScoreDigits() { fillDigits(scoreDigits, 5, score); }
void refreshTimeDigits()  { fillDigits(timeDigits, 3, timeLeft); }

void queueBlockErase(int32_t section, uint8_t index, uint8_t type) {
  if (pendingEraseCount >= MAX_ERASE) return;
  EraseRect& r = pendingErase[pendingEraseCount++];
  r.x = section * SECTION_W + (int16_t)pgm_read_word(&WORLD[index].x);
  r.y = pgm_read_byte(&WORLD[index].y);
  r.w = objWidth(type);
  r.h = objHeight(type);
}

// Record a gone WORLD slot and queue a one-shot GRAM rewrite of its rect.
// composeColumn skips it afterward so dirty paints do not put it back.
void markGone(int32_t section, uint8_t index, uint8_t type) {
  if (isBroken(section, index)) return;

  if (brokenCount < MAX_BROKEN) {
    brokenBricks[brokenCount].section = (int16_t)section;
    brokenBricks[brokenCount].index = index;
    brokenCount++;
  } else {
    // Ring: drop the oldest. Its GRAM hole stays until that column is
    // dirtied again, at which point the object can reappear - rare.
    for (uint8_t i = 1; i < MAX_BROKEN; i++) brokenBricks[i - 1] = brokenBricks[i];
    brokenBricks[MAX_BROKEN - 1].section = (int16_t)section;
    brokenBricks[MAX_BROKEN - 1].index = index;
  }

  queueBlockErase(section, index, type);
}

void bustBrick(int32_t section, uint8_t index) {
  markGone(section, index, O_BLOCK);
  audioPlay(SFX_BREAK);
}

void collectCoin(int32_t section, uint8_t index) {
  if (isBroken(section, index)) return;
  markGone(section, index, O_COIN);
  if (score <= 65535 - COIN_POINTS) score += COIN_POINTS;
  else score = 65535;
  refreshScoreDigits();
  audioPlay(SFX_COIN);
}

void spawnMushroom(int32_t worldX, int16_t blockY) {
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
void hitQBlock(int32_t section, uint8_t index) {
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

void growMario() {
  if (bigMario) return;
  bigMario = true;
  // Keep feet planted while the hitbox grows upward.
  playerYq -= (int32_t)(PLAYER_H_BIG - PLAYER_H_SMALL) << 8;
  audioPlay(SFX_POWERUP);
}

// --- Physics --------------------------------------------------------------

// Walks the solids in the three sections around a box. Returns true on
// any overlap, and reports the shallowest lift that would clear all of
// them - enough for enemies, which only ever land on things.
bool boxVsSolids(int32_t xq, int32_t yq, int32_t wq, int32_t hq, int32_t* liftOut) {
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
void resolveSolids(bool canBust) {
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

// --- Enemy logic ----------------------------------------------------------

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
  refreshTimeDigits();

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) enemies[i].type = E_NONE;
  for (uint8_t i = 0; i < MAX_ITEMS; i++) items[i].type = IT_NONE;
  spawnFrontier = 0;
  brokenCount = 0;
  usedQCount = 0;
  pendingEraseCount = 0;
  panelValid = false;
}

// Fatal hit / time-up: freeze, then death hop. Ignores power-up state.
void forceDeath() {
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
void playerHit() {
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
  refreshTimeDigits();
  if (timeLeft == 0) forceDeath();
}

void endDeath() {
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
void integrateActor(int32_t& xq, int32_t& yq, int16_t& vxq, int16_t& vyq,
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

void updateEnemy(Enemy& e) {
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
void defeatEnemy(Enemy& e) {
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

void updateItem(Item& it) {
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

// --- Column compositor ----------------------------------------------------

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

void vspan(uint16_t* b, int16_t y0, int16_t y1, uint16_t c) {
  if (y0 < clipTop) y0 = clipTop;
  if (y1 > clipBot) y1 = clipBot;
  if (y0 > y1) return;
  uint16_t* p = b + gramCol(y0, y1);
  for (int16_t n = y1 - y0 + 1; n > 0; n--) *p++ = c;
}

void buildCircTab() {
  for (uint8_t r = 0; r < 12; r++) {
    for (uint8_t d = 0; d < 12; d++) {
      int16_t rem = (int16_t)(r * r) - (int16_t)(d * d);
      uint8_t hw = 0;
      if (rem >= 0) {
        while ((int16_t)((hw + 1) * (hw + 1)) <= rem) hw++;
      }
      circTab[r][d] = hw;
    }
  }
}

void colDisc(uint16_t* b, int16_t du, uint8_t r, int16_t cy, uint16_t color) {
  if (du < 0) du = -du;
  if (du > r) return;
  uint8_t hh = circTab[r][du];
  vspan(b, cy - hh, cy + hh, color);
}

void colCloud(uint16_t* b, int16_t u, int16_t cy) {
  colDisc(b, u - 6, 5, cy + 5, WHITE);
  colDisc(b, u - 13, 7, cy + 2, WHITE);
  colDisc(b, u - 21, 5, cy + 5, WHITE);
  if (u >= 6 && u <= 21) vspan(b, cy + 5, cy + 10, WHITE);
}

// Apex at the top, base on the ground: the span starts T rows below
// the apex, where T is the first row wide enough to reach this column.
void colHill(uint16_t* b, int16_t u, uint16_t color) {
  int16_t du = u - 25;
  if (du < 0) du = -du;
  int16_t t = (du * 6 + 4) / 5;
  if (t <= 30) vspan(b, GROUND_Y - 30 + t, GROUND_Y - 1, color);
  colDisc(b, du, 11, GROUND_Y - 19, color);
}

void colBlock(uint16_t* b, int16_t u, int16_t by, bool question) {
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

void colPipe(uint16_t* b, int16_t u) {
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

void colCoin(uint16_t* b, int16_t u, int16_t cy) {
  colDisc(b, u - 4, 4, cy + 5, YELLOW);
  if (u == 4) vspan(b, cy + 2, cy + 7, WHITE);
}

// 3x5 digits, one byte per column, bit0 = top row. Gap column is empty.
const uint8_t FONT3x5[] PROGMEM = {
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
void composeHudDigit(uint16_t* b, int16_t local, uint8_t dig, uint16_t color) {
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

// Screen-fixed score (left) and timer (right) in the sky strip.
void composeHud(uint16_t* b, int32_t worldX) {
  int16_t sx = (int16_t)(worldX - cameraX);
  if (sx >= SCORE_HUD_X && sx < SCORE_HUD_X + SCORE_HUD_W) {
    int16_t local = sx - SCORE_HUD_X;
    composeHudDigit(b, local, scoreDigits[local / HUD_DIGIT_W], WHITE);
  } else if (sx >= TIME_HUD_X && sx < TIME_HUD_X + TIME_HUD_W) {
    int16_t local = sx - TIME_HUD_X;
    composeHudDigit(b, local, timeDigits[local / HUD_DIGIT_W],
                    timeLeft <= 100 ? RED : WHITE);
  }
}

void composeColumn(int32_t worldX, uint16_t* b) {
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
void spritePart(uint16_t* b, int16_t lu, int16_t top, int16_t lx, int16_t ly,
                int16_t w, int16_t h, int16_t sw, bool flip, uint16_t color) {
  int16_t x0 = flip ? (sw - lx - w) : lx;
  if (lu < x0 || lu >= x0 + w) return;
  vspan(b, top + ly, top + ly + h - 1, color);
}

void colGoomba(uint16_t* b, int16_t u, int16_t top, uint8_t frame) {
  spritePart(b, u, top, 1, 0, 12, 10, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 3, 4, 2, 3, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 9, 4, 2, 3, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 4, 5, 1, 2, GOOMBA_W, false, BLACK);
  spritePart(b, u, top, 9, 5, 1, 2, GOOMBA_W, false, BLACK);
  int16_t fx = frame ? 2 : 0;
  spritePart(b, u, top, fx, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
  spritePart(b, u, top, 9 - fx, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
}

void colSquashed(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 14, 4, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 0, 4, 14, 2, GOOMBA_W, false, GOOMBA_FT);
}

void colShellBody(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 13, 9, KOOPA_W, false, SHELL_GRN);
  spritePart(b, u, top, 2, 2, 9, 5, KOOPA_W, false, SHELL_LT);
  spritePart(b, u, top, 0, 9, 13, 3, KOOPA_W, false, SHELL_RIM);
}

void colKoopa(uint16_t* b, int16_t u, int16_t top, uint8_t frame, bool flip) {
  spritePart(b, u, top, 3, 0, 7, 7, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 6, 1, 3, 3, KOOPA_W, flip, WHITE);
  spritePart(b, u, top, 7, 2, 1, 2, KOOPA_W, flip, BLACK);
  colShellBody(b, u, top + 7);
  int16_t fx = frame ? 1 : 0;
  spritePart(b, u, top, fx, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 8 - fx, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
}

void composeEnemies(uint16_t* b, int32_t worldX) {
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

void runnerPart(uint16_t* b, int16_t lu, int16_t py, int16_t lx, int16_t ly,
                int16_t w, int16_t h, uint16_t color) {
  spritePart(b, lu, py, lx, ly, w, h, PLAYER_W, !facingRight, color);
}

void colMushroom(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 0, 0, 14, 7, MUSH_W, false, RED);
  spritePart(b, u, top, 3, 2, 2, 2, MUSH_W, false, WHITE);
  spritePart(b, u, top, 9, 3, 2, 2, MUSH_W, false, WHITE);
  spritePart(b, u, top, 4, 7, 6, 5, MUSH_W, false, SKIN);
  spritePart(b, u, top, 2, 12, 4, 2, MUSH_W, false, SKIN);
  spritePart(b, u, top, 8, 12, 4, 2, MUSH_W, false, SKIN);
}

void composeItems(uint16_t* b, int32_t worldX) {
  for (uint8_t i = 0; i < MAX_ITEMS; i++) {
    Item& it = items[i];
    if (it.type == IT_NONE || it.state == IS_GONE) continue;

    int32_t left = it.xq >> 8;
    if (worldX < left || worldX >= left + MUSH_W) continue;

    colMushroom(b, (int16_t)(worldX - left), (int16_t)(it.yq >> 8));
  }
}

void composeRunner(uint16_t* b, int32_t worldX) {
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

// --- Output ---------------------------------------------------------------

// World column -> GRAM row. Consecutive columns still land on
// consecutive rows, so the ring buffer and the one-column-per-pixel
// pan survive; reversing the direction is what cancels a mirrored
// panel axis, and setStartLine() carries the matching offset.
inline uint8_t gramRow(int32_t worldX) {
#if SCROLL_REVERSE
  return (uint8_t)((128 - (worldX & 127)) & 127);
#else
  return (uint8_t)(worldX & 127);
#endif
}

void pushColumn(int32_t worldX, int16_t y0, int16_t y1) {
  uint8_t c0 = gramCol(y0, y1);
  uint8_t len = (uint8_t)(y1 - y0 + 1);
  tft.setAddrWindow(c0, gramRow(worldX), len, 1);
  tft.writePixels(colBuf + c0, len);
#if DEBUG_SERIAL
  pixelCount += len;
#endif
}

void paintColumn(int32_t worldX, int16_t y0, int16_t y1) {
  clipTop = y0;
  clipBot = y1;
  composeColumn(worldX, colBuf);
  composeEnemies(colBuf, worldX);
  composeItems(colBuf, worldX);
  composeRunner(colBuf, worldX);
  pushColumn(worldX, y0, y1);
}

void setStartLine(int32_t cam) {
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
bool paintActorRect(int32_t cam, int32_t& prevX, int16_t& prevY,
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

void paintEnemyRects(int32_t cam) {
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

void syncEnemyRects() {
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

void paintItemRects(int32_t cam) {
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

void syncItemRects() {
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

// --- Immediate-mode UI (Adafruit GFX default font) ------------------------

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

void enterMode(uint8_t mode) {
  gameMode = mode;
  modeMs = millis();
  uiDirty = true;
  // Pad lines often read "pressed" on boot / mode change; wait for a
  // clean release before Start/A can advance the menu again.
  menuArmed = false;

  if (mode == MODE_TITLE) {
    score = 0;
    refreshScoreDigits();
    timeLeft = START_TIME;
    timeTick = 0;
    refreshTimeDigits();
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

void uiFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  tft.fillRect(x, y, w, h, color);
}

void uiClear() {
  tft.fillScreen(BLACK);
}

uint8_t uiStrLen_P(const __FlashStringHelper* fs) {
  const char* s = (const char*)fs;
  uint8_t n = 0;
  while (pgm_read_byte(s++)) n++;
  return n;
}

void uiPrint_P(int16_t x, int16_t y, const __FlashStringHelper* fs,
               uint16_t color, uint8_t size) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  tft.setCursor(x, y);
  tft.print(fs);
}

void uiCenter(int16_t y, const __FlashStringHelper* s, uint16_t color, uint8_t size) {
  int16_t x = (SCREEN_WIDTH - (int16_t)uiStrLen_P(s) * 6 * size) / 2;
  uiPrint_P(x, y, s, color, size);
}

void uiMenuCursor(int16_t x, int16_t y, bool on) {
  if (on) uiFill(x, y + 2, 3, 2, YELLOW), uiFill(x + 3, y + 1, 3, 4, YELLOW);
}

void uiRunnerSwatch(int16_t x, int16_t y, bool luigi) {
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

void uiMenuRow(int16_t y, const __FlashStringHelper* label, uint16_t color,
               bool selected, bool luigiSwatch) {
  uiMenuCursor(8, y + 8, selected);
  uiRunnerSwatch(22, y, luigiSwatch);
  uiPrint_P(44, y + 8, label, selected ? color : UI_DIM, 1);
  if (selected) uiFill(44, y + 18, 36, 1, color);
}

void drawTitle() {
  uiClear();
  uiCenter(18, F("SUPER"), WHITE, 2);
  uiCenter(38, F("MARDUINO"), RED, 2);
  uiCenter(58, F("BROS"), WHITE, 2);
  uiMenuCursor(22, 88, true);
  uiPrint_P(34, 88, F("PRESS START"), YELLOW, 1);
  if (!controllerOk) uiCenter(108, F("NO PAD"), RED, 1);
}

void drawSelect() {
  uiClear();
  uiCenter(8, F("SELECT PLAYER"), WHITE, 1);
  uiMenuRow(36, F("MARIO"), RED, selectIdx == 0, false);
  uiMenuRow(72, F("LUIGI"), LUIGI_GRN, selectIdx == 1, true);
  uiCenter(112, F("START TO CONFIRM"), UI_DIM, 1);
}

void drawLives() {
  uiClear();
  if (playAsLuigi) uiCenter(36, F("LUIGI"), LUIGI_GRN, 2);
  else             uiCenter(36, F("MARIO"), RED, 2);
  char line[4] = {'x', ' ', (char)('0' + lives), '\0'};
  tft.setTextSize(2);
  tft.setTextColor(WHITE);
  tft.setCursor((SCREEN_WIDTH - 36) / 2, 68);
  tft.print(line);
}

void drawDead() {
  uiClear();
  uiCenter(40, F("GAME OVER"), RED, 2);
  uiMenuCursor(22, 78, true);
  uiPrint_P(34, 78, F("PRESS START"), YELLOW, 1);
}

void paintUiIfDirty(void (*draw)()) {
  if (!uiDirty) return;
  draw();
  uiDirty = false;
}

bool menuConfirm(bool eStart, bool eA, bool startDown, bool aDown) {
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

void setup(void) {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif
  buildCircTab();

  tft.begin();  // library default is already 8 MHz, the AVR ceiling

  // Same remap Adafruit uses for rotation 0, minus the bottom-up scan
  // bit, so GRAM row N lands on panel row N and the start line offset
  // stays a straightforward add.
  applyGameRemap();

  tft.fillScreen(BLACK);

  controllerOk = initController();
#if DEBUG_SERIAL
  Serial.println(controllerOk ? F("NES pad found at 0x52")
                              : F("NES pad NOT found (check SDA/SCL/3.3V)"));
  Serial.println(F("Mount display rotated 90 deg counter-clockwise"));
  lastReport = millis();
#endif

  audioInit();  // boot blip on D9 if the piezo is wired

  lastStep = millis();

  enterMode(MODE_TITLE);
}

void loop() {
  uint32_t now = millis();
  audioUpdate();
  Buttons btn = readController();

  bool eStart = btn.start && !prevStart;
  bool eA     = btn.a && !prevA;
  bool eLeft  = btn.left && !prevLeft;
  bool eRight = btn.right && !prevRight;
  bool eUp    = btn.up && !prevUp;
  bool eDown  = btn.down && !prevDown;

  prevStart = btn.start;
  prevA     = btn.a;
  prevLeft  = btn.left;
  prevRight = btn.right;
  prevUp    = btn.up;
  prevDown  = btn.down;

  if (gameMode != MODE_PLAY) {
    switch (gameMode) {
      case MODE_TITLE:
        updateTitle(eStart, eA, btn.start, btn.a);
        break;
      case MODE_SELECT:
        updateSelect(eStart, eA, eLeft, eRight, eUp, eDown, btn.start, btn.a);
        break;
      case MODE_LIVES:
        updateLives(eStart, btn.start, btn.a);
        break;
      case MODE_DEAD:
        updateDead(eStart, btn.start, btn.a);
        break;
      default:
        break;
    }
    return;
  }

  // Start toggles pause only while alive. Death/pause keep lastStep pinned
  // so catch-up steps do not burst when gameplay resumes.
  if (playState == PLAY_PAUSE) {
    lastStep = now;
    if (eStart) {
      playState = PLAY_RUN;
      audioSetPaused(false);
      audioPlay(SFX_PAUSE);
    }
    return;
  }

  if (playState == PLAY_DEATH_HOLD) {
    lastStep = now;
    if (deathNeedsRender) {
      render();
      deathNeedsRender = false;
#if DEBUG_SERIAL
      frameCount++;
#endif
    }
    if (now - deathMs >= DEATH_HOLD_MS) {
      playState = PLAY_DEATH_FALL;
      onGround = false;
      velXq = 0;
      velYq = JUMP_VEL_Q;
      lastStep = now;
    }
    return;
  }

  if (playState == PLAY_DEATH_FALL) {
    if (now - lastStep > 250) lastStep = now;

    uint8_t steps = 0;
    while ((now - lastStep) >= STEP_MS && steps < 3) {
      updateDeathFall();
      lastStep += STEP_MS;
      steps++;
      if (playState != PLAY_DEATH_FALL) break;
    }

    if (gameMode != MODE_PLAY) return;

    if (steps) {
      render();
#if DEBUG_SERIAL
      frameCount++;
#endif
    }
  } else {
    // PLAY_RUN
    if (eStart) {
      playState = PLAY_PAUSE;
      lastStep = now;
      audioSetPaused(true);
      audioPlay(SFX_PAUSE);
      return;
    }

    if (now - lastStep > 250) lastStep = now;

    uint8_t steps = 0;
    while ((now - lastStep) >= STEP_MS && steps < 3) {
      // Re-read each step so held directions stay live across catch-up.
      updatePlayer(readController());
      spawnEnemies();
      updateEnemies();
      updateItems();
      collideShellHits();
      collideEnemies();
      collideItems();
      collideCoins();
      tickTimer();
      lastStep += STEP_MS;
      steps++;
      if (playState != PLAY_RUN) break;
    }

    if (gameMode != MODE_PLAY) return;

    // Fatal hit: paint the frozen mistake frame before the hold timer.
    if (playState == PLAY_DEATH_HOLD) {
      if (deathNeedsRender) {
        render();
        deathNeedsRender = false;
#if DEBUG_SERIAL
        frameCount++;
#endif
      }
      return;
    }

    if (steps) {
      render();
#if DEBUG_SERIAL
      frameCount++;
#endif
    }
  }

#if DEBUG_SERIAL
  if (now - lastReport >= 1000) {
    Serial.print(frameCount);
    Serial.print(F(" fps, "));
    Serial.print(frameCount ? (pixelCount / frameCount) : 0);
    Serial.println(F(" px/frame"));
    lastReport = now;
    frameCount = 0;
    pixelCount = 0;
  }
#endif
}
