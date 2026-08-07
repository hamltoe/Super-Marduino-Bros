/***************************************************
  Mario-style demo - SSD1351 128x128 OLED on Arduino Nano

  Controls: NES Classic / clone controller over I2C
    VCC -> 3.3V, GND -> GND, SDA -> A4, SCL -> A5

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

// Menu text sideways / upside-down? UI no longer uses setRotation; it
// paints in the same GRAM mapping as gameplay so menus stay upright
// on the mounted panel. Left here only as a note for older experiments.
// #define UI_ROTATION 1

// Drop to 100000 if a long controller cable gets flaky.
#define I2C_CLOCK 400000

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
const int16_t PLAYER_H = 22;
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

const int16_t MOVE_SPEED_Q = VEL_Q(50);    // 50 px/s run
const int16_t GRAVITY_Q    = ACC_Q(214);   // 214 px/s^2
const int16_t JUMP_VEL_Q   = -VEL_Q(130);  // 130 px/s launch, ~40 px of air
const int16_t MAX_FALL_Q   = VEL_Q(150);   // terminal velocity
const int16_t STOMP_VEL_Q  = -VEL_Q(93);   // bounce off a stomped enemy
const int32_t PLAYER_W_Q   = (int32_t)PLAYER_W << 8;
const int32_t PLAYER_H_Q   = (int32_t)PLAYER_H << 8;

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
  {166, (uint8_t)(GROUND_Y - 34), O_PIPE},
  {230, 75, O_QBLOCK},
  {267, 48, O_BLOCK},
  {282, 48, O_BLOCK},
};
const uint8_t WORLD_COUNT = sizeof(WORLD) / sizeof(WORLD[0]);

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

// World x already scanned for spawns. It only ever moves right, so
// walking back over old ground does not repopulate it.
int32_t spawnFrontier = 0;
uint8_t animTick = 0;
uint8_t enemyFrame = 0;

struct Buttons {
  bool up, down, left, right;
  bool a, b, select, start;
};

int32_t playerXq = (int32_t)40 << 8;
int32_t playerYq = (int32_t)(GROUND_Y - PLAYER_H) << 8;
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
uint32_t lastReport = 0;
uint16_t frameCount = 0;
uint32_t pixelCount = 0;

// --- Game flow ------------------------------------------------------------

#define START_LIVES    3
#define LIVES_HOLD_MS  2000

enum : uint8_t {
  MODE_TITLE = 0,
  MODE_SELECT,
  MODE_LIVES,
  MODE_PLAY,
  MODE_DEAD
};

uint8_t gameMode = MODE_TITLE;
uint8_t lives = START_LIVES;
bool playAsLuigi = false;
uint8_t selectIdx = 0;   // 0 = Mario, 1 = Luigi
uint32_t modeMs = 0;
bool uiDirty = true;
bool menuArmed = false;  // ignore confirm until Start/A have been released

bool prevStart = false;
bool prevA = false;
bool prevLeft = false;
bool prevRight = false;
bool prevUp = false;
bool prevDown = false;

void enterMode(uint8_t mode);

// Classic Adafruit 5x7 glyphs for ' '..'Z' (uppercase menus + digits).
// Same layout as Adafruit_GFX glcdfont: 5 column bytes, bit0 = top.
const uint8_t UI_FONT[] PROGMEM = {
  0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00,
  0x14,0x7F,0x14,0x7F,0x14, 0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62,
  0x36,0x49,0x56,0x20,0x50, 0x00,0x08,0x07,0x03,0x00, 0x00,0x1C,0x22,0x41,0x00,
  0x00,0x41,0x22,0x1C,0x00, 0x2A,0x1C,0x7F,0x1C,0x2A, 0x08,0x08,0x3E,0x08,0x08,
  0x00,0x80,0x70,0x30,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x00,0x60,0x60,0x00,
  0x20,0x10,0x08,0x04,0x02, 0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00,
  0x72,0x49,0x49,0x49,0x46, 0x21,0x41,0x49,0x4D,0x33, 0x18,0x14,0x12,0x7F,0x10,
  0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x31, 0x41,0x21,0x11,0x09,0x07,
  0x36,0x49,0x49,0x49,0x36, 0x46,0x49,0x49,0x29,0x1E, 0x00,0x00,0x14,0x00,0x00,
  0x00,0x40,0x34,0x00,0x00, 0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14,
  0x00,0x41,0x22,0x14,0x08, 0x02,0x01,0x59,0x09,0x06, 0x3E,0x41,0x5D,0x59,0x4E,
  0x7C,0x12,0x11,0x12,0x7C, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
  0x7F,0x41,0x41,0x41,0x3E, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x09,0x01,
  0x3E,0x41,0x41,0x51,0x73, 0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00,
  0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41, 0x7F,0x40,0x40,0x40,0x40,
  0x7F,0x02,0x1C,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
  0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46,
  0x26,0x49,0x49,0x49,0x32, 0x03,0x01,0x7F,0x01,0x03, 0x3F,0x40,0x40,0x40,0x3F,
  0x1F,0x20,0x40,0x20,0x1F, 0x3F,0x40,0x38,0x40,0x3F, 0x63,0x14,0x08,0x14,0x63,
  0x03,0x04,0x78,0x04,0x03, 0x61,0x59,0x49,0x4D,0x43
};

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

uint8_t objWidth(uint8_t t) {
  switch (t) {
    case O_CLOUD: return 28;
    case O_HILL:
    case O_HILL2: return 51;
    case O_COIN:  return 9;
    case O_PIPE:  return 24;
    default:      return 15;
  }
}

uint8_t objHeight(uint8_t t) {
  return (t == O_PIPE) ? 34 : 15;
}

bool objSolid(uint8_t t) {
  return t == O_BLOCK || t == O_QBLOCK || t == O_PIPE;
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
    int32_t origin = (baseSection + s) * SECTION_W;

    for (uint8_t i = 0; i < WORLD_COUNT; i++) {
      uint8_t type = pgm_read_byte(&WORLD[i].type);
      if (!objSolid(type)) continue;

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

void resolveSolids() {
  int16_t playerPx = (int16_t)(playerXq >> 8);
  int32_t baseSection = ((int32_t)playerPx / SECTION_W) - 1;
  if (playerPx < 0) baseSection--;

  for (int8_t s = 0; s < 3; s++) {
    int32_t origin = (baseSection + s) * SECTION_W;

    for (uint8_t i = 0; i < WORLD_COUNT; i++) {
      uint8_t type = pgm_read_byte(&WORLD[i].type);
      if (!objSolid(type)) continue;

      int32_t sx = origin + (int16_t)pgm_read_word(&WORLD[i].x);
      int16_t sw = objWidth(type);

      int32_t gap = sx - playerPx;
      if (gap > PLAYER_W || gap < -sw) continue;

      int32_t sxq = sx << 8;
      int32_t syq = (int32_t)pgm_read_byte(&WORLD[i].y) << 8;
      int32_t swq = (int32_t)sw << 8;
      int32_t shq = (int32_t)objHeight(type) << 8;

      if (playerXq >= sxq + swq || playerXq + PLAYER_W_Q <= sxq) continue;
      if (playerYq >= syq + shq || playerYq + PLAYER_H_Q <= syq) continue;

      int32_t overlapL = (playerXq + PLAYER_W_Q) - sxq;
      int32_t overlapR = (sxq + swq) - playerXq;
      int32_t overlapT = (playerYq + PLAYER_H_Q) - syq;
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

  bool jumpHeld = btn.a || btn.b;
  if (jumpHeld && !jumpWasHeld && onGround) {
    velYq = JUMP_VEL_Q;
    onGround = false;
  }
  jumpWasHeld = jumpHeld;

  velYq += GRAVITY_Q;
  if (velYq > MAX_FALL_Q) velYq = MAX_FALL_Q;

  playerXq += velXq;
  if (playerXq < 0) {
    playerXq = 0;
    velXq = 0;
  }
  resolveSolids();

  playerYq += velYq;
  onGround = false;
  resolveSolids();

  int32_t floorQ = (int32_t)(GROUND_Y - PLAYER_H) << 8;
  if (playerYq >= floorQ) {
    playerYq = floorQ;
    velYq = 0;
    onGround = true;
  }

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
  playerXq = (int32_t)40 << 8;
  playerYq = (int32_t)(GROUND_Y - PLAYER_H) << 8;
  velXq = 0;
  velYq = 0;
  onGround = true;
  facingRight = true;
  jumpWasHeld = false;
  cameraX = 0;

  for (uint8_t i = 0; i < MAX_ENEMIES; i++) enemies[i].type = E_NONE;
  spawnFrontier = 0;
  panelValid = false;
}

void playerHit() {
  if (lives > 1) {
    lives--;
    enterMode(MODE_LIVES);
  } else {
    lives = 0;
    enterMode(MODE_DEAD);
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

void updateEnemy(Enemy& e) {
  if (e.state == ES_SQUASH) {
    if (--e.timer == 0) e.state = ES_GONE;
    return;
  }

  int32_t wq = (int32_t)enemyWidth(e) << 8;
  int32_t hq = (int32_t)enemyHeight(e) << 8;

  if (e.vxq) {
    e.xq += e.vxq;
    if (boxVsSolids(e.xq, e.yq, wq, hq, NULL)) {
      e.xq -= e.vxq;
      e.vxq = -e.vxq;
    }
  }

  e.vyq += GRAVITY_Q;
  if (e.vyq > MAX_FALL_Q) e.vyq = MAX_FALL_Q;
  e.yq += e.vyq;

  int32_t lift;
  if (boxVsSolids(e.xq, e.yq, wq, hq, &lift)) {
    e.yq -= lift;
    if (e.vyq > 0) e.vyq = 0;
  }

  int32_t floorQ = (int32_t)(GROUND_Y - (hq >> 8)) << 8;
  if (e.yq >= floorQ) {
    e.yq = floorQ;
    e.vyq = 0;
  }
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

void collideEnemies() {
  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE || e.state == ES_GONE || e.state == ES_SQUASH) continue;

    int32_t wq = (int32_t)enemyWidth(e) << 8;
    int32_t hq = (int32_t)enemyHeight(e) << 8;

    if (playerXq >= e.xq + wq || playerXq + PLAYER_W_Q <= e.xq) continue;
    if (playerYq >= e.yq + hq || playerYq + PLAYER_H_Q <= e.yq) continue;

    // Coming down onto the top half counts as a stomp; anything else
    // is a hit, except for a shell sitting still, which gets kicked.
    bool stomp = velYq > 0 && (playerYq + PLAYER_H_Q - e.yq) <= (hq >> 1);

    if (stomp) {
      playerYq = e.yq - PLAYER_H_Q;
      velYq = STOMP_VEL_Q;
      onGround = false;

      if (e.type == E_GOOMBA) {
        e.state = ES_SQUASH;
        e.timer = SQUASH_TICKS;
        e.vxq = 0;
        e.yq += (int32_t)(GOOMBA_H - SQUASH_H) << 8;
      } else if (e.state == ES_WALK) {
        e.state = ES_SHELL;
        e.vxq = 0;
        e.yq += (int32_t)(KOOPA_H - SHELL_H) << 8;
      } else if (e.state == ES_SLIDE) {
        e.state = ES_SHELL;
        e.vxq = 0;
      } else {
        e.state = ES_SLIDE;
        e.vxq = facingRight ? SHELL_SPEED_Q : -SHELL_SPEED_Q;
      }
    } else if (e.state == ES_SHELL) {
      bool fromLeft = playerXq < e.xq;
      e.state = ES_SLIDE;
      e.vxq = fromLeft ? SHELL_SPEED_Q : -SHELL_SPEED_Q;
      playerXq += fromLeft ? -(int32_t)(3 << 8) : (int32_t)(3 << 8);
    } else {
      playerHit();
      return;
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

void composeColumn(int32_t worldX, uint16_t* b) {
  vspan(b, 0, GROUND_Y - 1, SKY_BLUE);
  vspan(b, GROUND_Y, GROUND_Y + 3, GRASS);
  if ((worldX & 15) == 0) {
    vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, DARK_DIRT);
  } else {
    vspan(b, GROUND_Y + 4, SCREEN_HEIGHT - 1, DIRT);
    vspan(b, GROUND_Y + 15, GROUND_Y + 15, DARK_DIRT);
  }

  int16_t wx = (int16_t)(worldX % SECTION_W);

  for (uint8_t i = 0; i < WORLD_COUNT; i++) {
    uint8_t type = pgm_read_byte(&WORLD[i].type);
    int16_t u = wx - (int16_t)pgm_read_word(&WORLD[i].x);
    if (u < 0 || u >= (int16_t)objWidth(type)) continue;

    int16_t oy = pgm_read_byte(&WORLD[i].y);
    switch (type) {
      case O_CLOUD:  colCloud(b, u, oy); break;
      case O_HILL:   colHill(b, u, HILL_GREEN); break;
      case O_HILL2:  colHill(b, u, HILL_LIGHT); break;
      case O_COIN:   colCoin(b, u, oy); break;
      case O_PIPE:   colPipe(b, u); break;
      case O_QBLOCK: colBlock(b, u, oy, true); break;
      default:       colBlock(b, u, oy, false); break;
    }
  }

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
  spritePart(b, u, top, 3, 0, 8, 2, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 1, 2, 12, 2, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 0, 4, 14, 6, GOOMBA_W, false, GOOMBA_BR);

  spritePart(b, u, top, 3, 5, 2, 4, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 9, 5, 2, 4, GOOMBA_W, false, WHITE);
  spritePart(b, u, top, 4, 6, 1, 3, GOOMBA_W, false, BLACK);
  spritePart(b, u, top, 9, 6, 1, 3, GOOMBA_W, false, BLACK);

  spritePart(b, u, top, 2, 10, 10, 2, GOOMBA_W, false, DARK_DIRT);

  if (frame == 0) {
    spritePart(b, u, top, 0, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
    spritePart(b, u, top, 9, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
  } else {
    spritePart(b, u, top, 2, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
    spritePart(b, u, top, 7, 12, 5, 2, GOOMBA_W, false, GOOMBA_FT);
  }
}

void colSquashed(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 1, 0, 12, 2, GOOMBA_W, false, GOOMBA_BR);
  spritePart(b, u, top, 0, 2, 14, 2, GOOMBA_W, false, DARK_DIRT);
  spritePart(b, u, top, 0, 4, 14, 2, GOOMBA_W, false, GOOMBA_FT);
  spritePart(b, u, top, 3, 0, 2, 2, GOOMBA_W, false, BLACK);
  spritePart(b, u, top, 9, 0, 2, 2, GOOMBA_W, false, BLACK);
}

void colShellBody(uint16_t* b, int16_t u, int16_t top) {
  spritePart(b, u, top, 1, 0, 11, 1, KOOPA_W, false, SHELL_GRN);
  spritePart(b, u, top, 0, 1, 13, 8, KOOPA_W, false, SHELL_GRN);
  spritePart(b, u, top, 2, 2, 9, 5, KOOPA_W, false, SHELL_LT);
  spritePart(b, u, top, 4, 3, 5, 3, KOOPA_W, false, SHELL_GRN);
  spritePart(b, u, top, 0, 9, 13, 3, KOOPA_W, false, SHELL_RIM);
}

void colKoopa(uint16_t* b, int16_t u, int16_t top, uint8_t frame, bool flip) {
  spritePart(b, u, top, 3, 0, 7, 6, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 9, 2, 3, 3, KOOPA_W, flip, KOOPA_SKIN);
  spritePart(b, u, top, 6, 1, 3, 3, KOOPA_W, flip, WHITE);
  spritePart(b, u, top, 7, 2, 1, 2, KOOPA_W, flip, BLACK);
  spritePart(b, u, top, 4, 6, 5, 2, KOOPA_W, flip, KOOPA_SKIN);

  colShellBody(b, u, top + 7);

  if (frame == 0) {
    spritePart(b, u, top, 0, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
    spritePart(b, u, top, 8, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
  } else {
    spritePart(b, u, top, 1, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
    spritePart(b, u, top, 7, 18, 5, 2, KOOPA_W, flip, KOOPA_SKIN);
  }
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
  int16_t x0 = facingRight ? lx : (PLAYER_W - lx - w);
  if (lu < x0 || lu >= x0 + w) return;
  vspan(b, py + ly, py + ly + h - 1, color);
}

void composeRunner(uint16_t* b, int32_t worldX) {
  int32_t left = playerXq >> 8;
  if (worldX < left || worldX >= left + PLAYER_W) return;

  int16_t lu = (int16_t)(worldX - left);
  int16_t py = (int16_t)(playerYq >> 8);
  uint16_t accent = playAsLuigi ? LUIGI_GRN : RED;

  runnerPart(b, lu, py, 3, 0, 8, 3, accent);
  runnerPart(b, lu, py, 1, 3, 12, 3, accent);
  runnerPart(b, lu, py, 4, 6, 7, 5, SKIN);
  runnerPart(b, lu, py, 9, 7, 2, 2, DARK_DIRT);
  runnerPart(b, lu, py, 2, 11, 10, 7, BLUE);
  runnerPart(b, lu, py, 0, 12, 3, 5, accent);
  runnerPart(b, lu, py, 11, 12, 3, 5, accent);

  if (!onGround) {
    runnerPart(b, lu, py, 1, 18, 4, 4, DARK_DIRT);
    runnerPart(b, lu, py, 9, 18, 4, 4, DARK_DIRT);
  } else if (animFrame == 0) {
    runnerPart(b, lu, py, 2, 18, 4, 4, DARK_DIRT);
    runnerPart(b, lu, py, 9, 18, 5, 3, DARK_DIRT);
  } else {
    runnerPart(b, lu, py, 0, 18, 5, 3, DARK_DIRT);
    runnerPart(b, lu, py, 9, 18, 4, 4, DARK_DIRT);
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
  pixelCount += len;
}

void paintColumn(int32_t worldX, int16_t y0, int16_t y1) {
  clipTop = y0;
  clipBot = y1;
  composeColumn(worldX, colBuf);
  composeEnemies(colBuf, worldX);
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

// Repaints what each enemy covered and now covers, world-first, the
// same way Mario's slice is handled. A slot marked ES_GONE is drawn
// out of its last rectangle and then freed.
void paintEnemyRects(int32_t cam) {
  for (uint8_t i = 0; i < MAX_ENEMIES; i++) {
    Enemy& e = enemies[i];
    if (e.type == E_NONE) continue;

    int32_t cx = e.xq >> 8;
    int16_t cy = (int16_t)(e.yq >> 8);
    bool gone = (e.state == ES_GONE);

    // A parked shell or a flattened Goomba has no animation, so once
    // it stops moving its pixels are already right.
    if (!gone && cx == e.prevX && cy == e.prevY &&
        (e.state == ES_SHELL || e.state == ES_SQUASH)) {
      continue;
    }

    int32_t x0 = gone ? e.prevX : min(e.prevX, cx);
    int32_t x1 = (gone ? e.prevX : max(e.prevX, cx)) + enemyWidth(e) - 1;
    int16_t y0 = gone ? e.prevY : min(e.prevY, cy);
    int16_t y1 = (gone ? e.prevY : max(e.prevY, cy)) + enemyMaxHeight(e) - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;

    for (int32_t wx = x0; wx <= x1; wx++) {
      if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
      paintColumn(wx, y0, y1);
    }

    e.prevX = cx;
    e.prevY = cy;
    if (gone) e.type = E_NONE;
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
    syncEnemyRects();
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
    // so his old pixels are erased in the same write.
    int32_t c0 = min(marioColPrev, marioCol);
    int32_t c1 = max(marioColPrev, marioCol) + PLAYER_W - 1;
    int16_t y0 = min(marioRowPrev, marioRow);
    int16_t y1 = max(marioRowPrev, marioRow) + PLAYER_H - 1;
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;

    for (int32_t wx = c0; wx <= c1; wx++) {
      if (wx < cam || wx > cam + SCREEN_WIDTH - 1) continue;
      paintColumn(wx, y0, y1);
    }

    paintEnemyRects(cam);

    tft.endWrite();
  }

  setStartLine(cam);
  panelCam = cam;
  marioColPrev = marioCol;
  marioRowPrev = marioRow;
}

// --- Immediate-mode UI (Adafruit GFX default font) ------------------------

void applyGameRemap() {
  uint8_t remap = 0b01100100;  // match setup(): no bottom-up scan
  tft.sendCommand(SSD1351_CMD_SETREMAP, &remap, 1);
  uint8_t zero = 0;
  tft.sendCommand(SSD1351_CMD_STARTLINE, &zero, 1);
}

void enterMode(uint8_t mode) {
  gameMode = mode;
  modeMs = millis();
  uiDirty = true;
  // Pad lines often read "pressed" on boot / mode change; wait for a
  // clean release before Start/A can advance the menu again.
  menuArmed = false;

  applyGameRemap();
  if (mode == MODE_PLAY) {
    resetLevel();
    lastStep = millis();
  }
}

// --- Immediate-mode UI (view-space, same mapping as gameplay) -------------

void uiFill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  int16_t x1 = x + w - 1;
  int16_t y1 = y + h - 1;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x1 > SCREEN_WIDTH - 1) x1 = SCREEN_WIDTH - 1;
  if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;
  if (x > x1 || y > y1) return;

  tft.startWrite();
  for (int16_t vx = x; vx <= x1; vx++) {
    clipTop = y;
    clipBot = y1;
    vspan(colBuf, y, y1, color);
    pushColumn(vx, y, y1);
  }
  tft.endWrite();
}

void uiClear() {
  uiFill(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, BLACK);
}

void uiChar(int16_t x, int16_t y, char ch, uint16_t color, uint8_t size) {
  if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
  if (ch < ' ' || ch > 'Z') return;

  uint16_t idx = (uint16_t)(ch - ' ') * 5;
  for (uint8_t col = 0; col < 5; col++) {
    uint8_t bits = pgm_read_byte(&UI_FONT[idx + col]);
    for (uint8_t row = 0; row < 7; row++) {
      if (!(bits & (1 << row))) continue;
      int16_t px = x + (int16_t)col * size;
      int16_t py = y + (int16_t)row * size;
      int16_t py1 = py + size - 1;
      for (uint8_t dx = 0; dx < size; dx++) {
        clipTop = py;
        clipBot = py1;
        vspan(colBuf, py, py1, color);
        pushColumn(px + dx, py, py1);
      }
    }
  }
}

uint8_t uiStrWidth(const char* s, uint8_t size) {
  uint8_t n = 0;
  while (s[n]) n++;
  return (uint8_t)(n * 6 * size);
}

void uiPrint(int16_t x, int16_t y, const char* s, uint16_t color, uint8_t size) {
  tft.startWrite();
  while (*s) {
    uiChar(x, y, *s++, color, size);
    x += 6 * size;
  }
  tft.endWrite();
}

void uiPrint_P(int16_t x, int16_t y, const __FlashStringHelper* fs,
               uint16_t color, uint8_t size) {
  const char* s = (const char*)fs;
  tft.startWrite();
  char ch;
  while ((ch = (char)pgm_read_byte(s++)) != 0) {
    uiChar(x, y, ch, color, size);
    x += 6 * size;
  }
  tft.endWrite();
}

uint8_t uiStrWidth_P(const __FlashStringHelper* fs, uint8_t size) {
  const char* s = (const char*)fs;
  uint8_t n = 0;
  while (pgm_read_byte(s++)) n++;
  return (uint8_t)(n * 6 * size);
}

void uiCenter(int16_t y, const __FlashStringHelper* s, uint16_t color, uint8_t size) {
  int16_t x = (SCREEN_WIDTH - (int16_t)uiStrWidth_P(s, size)) / 2;
  uiPrint_P(x, y, s, color, size);
}

void uiCenterCStr(int16_t y, const char* s, uint16_t color, uint8_t size) {
  int16_t x = (SCREEN_WIDTH - (int16_t)uiStrWidth(s, size)) / 2;
  uiPrint(x, y, s, color, size);
}

void uiMenuCursor(int16_t x, int16_t y, bool on) {
  if (on) uiPrint_P(x, y, F(">"), YELLOW, 1);
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

  char line[8];
  line[0] = 'x';
  line[1] = ' ';
  line[2] = (char)('0' + lives);
  line[3] = '\0';
  uiCenterCStr(68, line, WHITE, 2);
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
    enterMode(MODE_SELECT);
  }
}

void updateSelect(bool eStart, bool eA, bool eLeft, bool eRight,
                  bool eUp, bool eDown, bool startDown, bool aDown) {
  if (menuArmed) {
    if (eLeft || eUp) {
      selectIdx = 0;
      uiDirty = true;
    } else if (eRight || eDown) {
      selectIdx = 1;
      uiDirty = true;
    }
  }

  paintUiIfDirty(drawSelect);

  if (menuConfirm(eStart, eA, startDown, aDown)) {
    playAsLuigi = (selectIdx == 1);
    lives = START_LIVES;
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
    enterMode(MODE_TITLE);
  }
}

void setup(void) {
  Serial.begin(115200);
  buildCircTab();

  tft.begin();  // library default is already 8 MHz, the AVR ceiling

  // Same remap Adafruit uses for rotation 0, minus the bottom-up scan
  // bit, so GRAM row N lands on panel row N and the start line offset
  // stays a straightforward add.
  applyGameRemap();

  tft.fillScreen(BLACK);

  controllerOk = initController();
  Serial.println(controllerOk ? F("NES pad found at 0x52")
                              : F("NES pad NOT found (check SDA/SCL/3.3V)"));
  Serial.println(F("Mount display rotated 90 deg counter-clockwise"));

  lastStep = millis();
  lastReport = lastStep;

  enterMode(MODE_TITLE);
}

void loop() {
  uint32_t now = millis();
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

  if (now - lastStep > 250) lastStep = now;

  uint8_t steps = 0;
  while ((now - lastStep) >= STEP_MS && steps < 3) {
    // Re-read each step so held directions stay live across catch-up.
    updatePlayer(readController());
    spawnEnemies();
    updateEnemies();
    collideEnemies();
    lastStep += STEP_MS;
    steps++;
    if (gameMode != MODE_PLAY) break;
  }

  if (gameMode != MODE_PLAY) {
    // Death mid-frame: leave GRAM as-is until the UI draw runs next pass.
    return;
  }

  if (steps) {
    render();
    frameCount++;
  }

  if (now - lastReport >= 1000) {
    Serial.print(frameCount);
    Serial.print(F(" fps, "));
    Serial.print(frameCount ? (pixelCount / frameCount) : 0);
    Serial.println(F(" px/frame"));
    lastReport = now;
    frameCount = 0;
    pixelCount = 0;
  }
}
