/***************************************************
  Board pins, feature flags, colors, and shared
  compile-time constants. Prefer #define / enum so
  avr-gcc never parks a copy in SRAM (plain const
  data is RAM-backed on AVR unless PROGMEM).
 ****************************************************/
#ifndef SUPER_MARDUINO_CONFIG_H
#define SUPER_MARDUINO_CONFIG_H

#include <Arduino.h>

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

#define SKY_BLUE   0x4D7F
#define HILL_GREEN 0x2589
#define HILL_LIGHT 0x36CC
#define GRASS      0x4E20
#define DIRT       0xA285
#define DARK_DIRT  0x6182
#define ORANGE     0xFC60
#define PIPE_HI    0x87E0
#define SKIN       0xFD20
#define GOOMBA_BR  0xAD04
#define GOOMBA_FT  0xF64F
#define KOOPA_SKIN 0xFECF
#define SHELL_GRN  0x0404
#define SHELL_LT   0x770B
#define SHELL_RIM  0xFF14
#define LUIGI_GRN  0x07C0
#define UI_DIM     0x8410

#define GROUND_Y       104
// Finite World 1-1: one section spans the whole course (no wrap).
#define LEVEL_W        1200
#define SECTION_W      LEVEL_W
#define FLAG_X         1110
#define PLAYER_W       14
#define PLAYER_H_SMALL 14
#define PLAYER_H_BIG   22
#define CAMERA_MARGIN  40
#define TIME_BONUS     50

#define STEP_MS 33    // 16ms @ 60 fps, 33ms @ 30 fps

// The integrator works in 8.8 pixels per step, which means every
// motion constant has to be rescaled whenever STEP_MS changes - a
// velocity with the step, an acceleration with its square. Authoring
// them in real-world units and converting at compile time keeps the
// run speed and the jump arc identical at any frame rate.
#define VEL_Q(px_s)  ((int16_t)(((int32_t)(px_s) * 256 * STEP_MS + 500) / 1000))
#define ACC_Q(px_s2) ((int16_t)(((int32_t)(px_s2) * 256 * STEP_MS * STEP_MS \
                                 + 500000L) / 1000000L))

#define MOVE_SPEED_Q VEL_Q(50)     // 50 px/s walk; B run is 1.5x
#define GRAVITY_Q    ACC_Q(214)    // 214 px/s^2
#define JUMP_VEL_Q   (-VEL_Q(130)) // walk jump ~40 px; B adds VEL_Q(30)
#define MAX_FALL_Q   VEL_Q(150)    // terminal velocity
#define STOMP_VEL_Q  (-VEL_Q(93))  // bounce off a stomped enemy
#define PLAYER_W_Q   ((int32_t)PLAYER_W << 8)
#define INVULN_TICKS ((uint8_t)(2000 / STEP_MS))  // ~2 s after shrink

#define O_CLOUD  0
#define O_HILL   1
#define O_HILL2  2
#define O_BLOCK  3
#define O_QBLOCK 4
#define O_COIN   5
#define O_PIPE   6
#define O_FLAG   7

#define E_NONE   0
#define E_GOOMBA 1
#define E_KOOPA  2

#define ES_WALK   0
#define ES_SQUASH 1  // flattened Goomba, waits out a timer
#define ES_SHELL  2  // Koopa withdrawn and stationary
#define ES_SLIDE  3  // shell kicked and travelling
#define ES_GONE   4  // still on screen as stale pixels, erased next render

#define MAX_ENEMIES 6

#define GOOMBA_W 14
#define GOOMBA_H 14
#define KOOPA_W  13
#define KOOPA_H  20
#define SHELL_H  12
#define SQUASH_H 6

#define GOOMBA_SPEED_Q VEL_Q(19)
#define KOOPA_SPEED_Q  VEL_Q(16)
#define SHELL_SPEED_Q  VEL_Q(75)
#define SQUASH_TICKS   (640 / STEP_MS)  // ~0.64 s flattened
#define ANIM_TICKS     (130 / STEP_MS)  // ~0.13 s per walk frame

#define IT_NONE     0
#define IT_MUSHROOM 1

#define IS_RISE 0  // emerging from a just-hit ? block
#define IS_WALK 1
#define IS_GONE 2  // stale pixels; freed after dirty-rect erase

#define MAX_ITEMS 2

#define MUSH_W 14
#define MUSH_H 14
#define MUSH_SPEED_Q VEL_Q(19)
#define MUSH_RISE_Q  (-VEL_Q(22))

#define MAX_BROKEN 12
#define MAX_USED_Q 8
#define MAX_ERASE 4

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

#define START_LIVES    3
#define LIVES_HOLD_MS  2000
#define DEATH_HOLD_MS  450

enum : uint8_t {
  MODE_TITLE = 0,
  MODE_SELECT,
  MODE_LIVES,
  MODE_PLAY,
  MODE_DEAD,
  MODE_WIN
};

enum : uint8_t {
  PLAY_RUN = 0,
  PLAY_PAUSE,
  PLAY_DEATH_HOLD,   // frozen world - show the mistake
  PLAY_DEATH_FALL    // no-collision jump, then fall through the map
};

struct ObjDef {
  int16_t x;
  uint8_t y;
  uint8_t type;
};

struct EnemyDef {
  int16_t x;
  uint8_t type;
};

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

struct BrokenBrick {
  int16_t section;
  uint8_t index;
};

typedef BrokenBrick UsedQBlock;

struct EraseRect {
  int32_t x;
  int16_t y;
  uint8_t w;
  uint8_t h;
};

struct Buttons {
  bool up, down, left, right;
  bool a, b, select, start;
};

#endif
