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
  orientation - see FLIP_Y and SCROLL_REVERSE in Config.h.

  Modules (kept as .cpp/.h like Audio to stay flash-small):
    Config.h   - pins, colors, constants (header-only folds)
    Input      - NES Classic over I2C
    World      - PROGMEM level + gone/?-block state
    Game       - player, enemies, items, physics, score
    Display    - column compositor + SSD1351 output
    UI         - title / select / lives / game-over
    Audio      - piezo SFX/BGM (Timer1 CTC)
 ****************************************************/

#include "Config.h"
#include "Input.h"
#include "Game.h"
#include "Display.h"
#include "UI.h"
#include "Audio.h"

void setup(void) {
#if DEBUG_SERIAL
  Serial.begin(115200);
#endif
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
