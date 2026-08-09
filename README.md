# Mario-style scroller for the Arduino Nano

Playable Mario-style side-scroller on a 128×128 SSD1351 OLED, built for an ATmega328P-class board (Arduino Nano / Uno). The design centers on **hardware scrolling + column compositing** so SPI bandwidth stays within what the AVR can sustain.

## Hardware

| Part | Wiring / notes |
|------|----------------|
| **SSD1351 OLED** | Hardware SPI: CS → 10, DC → 7, RST → 8 (SCLK → 13, MOSI → 11). Mount the module **rotated 90° counter-clockwise**. |
| **NES Classic / clone** | I2C at `0x52`: VCC → 3.3V, GND → GND, SDA → A4, SCL → A5. Default clock `400000` (drop to `100000` if a long cable is flaky). |
| **Piezo / passive buzzer** | Signal → **D9**, other lead → GND. Must be *passive* (not a self-driving active buzzer). A short boot chirp plays on reset if wiring is good. |

If the picture is mirrored or sky is on the wrong side, toggle `FLIP_Y` in `Config.h`. If the world scrolls the wrong way relative to Mario, toggle `SCROLL_REVERSE` there too.

Without a controller, inputs stay idle and a red strip in early columns signals “no pad.”

## Why this architecture

SPI on an ATmega328P is capped at `F_CPU/2` = 8 MHz. A pixel costs ~2 µs, so a full 128×128 frame is ~33 ms of bus time. Full-screen software redraws flicker and crawl.

The SSD1351 **Set Display Start Line** register (`0xA1`) slides the panel over its own 128-row GRAM with wraparound for the cost of one command. That axis is the panel’s *vertical* one, so this renderer is transposed:

- One GRAM row holds one **column** of the game world.
- GRAM behaves as a **128-entry ring buffer** of world columns.
- Panning one pixel means writing the newly visible column(s) and bumping the start line.

The panel is square, so rotating the module 90° costs no screen area. Scrolling goes from ~16 000 pixels per frame to roughly one new world column plus a repaint of the ~15 columns Mario occupies.

## File structure

Everything lives under `Super_Marduino_Bros/`. Modules are `.h` / `.cpp` pairs (same pattern as Audio) so each area can be edited without scrolling a 2k-line sketch. Layout is chosen to stay flash-small on the Nano: PROGMEM data stays in one translation unit, headers declare only, and file-local helpers are `static`.

```
Super_Marduino_Bros/
  Super_Marduino_Bros.ino   setup() / loop() glue
  Config.h                  pins, colors, physics constants, shared structs
  Input.h / Input.cpp       NES Classic over I2C
  World.h / World.cpp       PROGMEM level + broken / ?-block state
  Game.h / Game.cpp         player, enemies, items, physics, score / timer
  Display.h / Display.cpp   column compositor + SSD1351 output
  UI.h / UI.cpp             title / select / lives / game-over
  Audio.h / Audio.cpp       piezo SFX + BGM (Timer1 CTC on D9)
```

| Module | Role |
|--------|------|
| **Config** | Board pins, feature flags (`FLIP_Y`, `SCROLL_REVERSE`, `DEBUG_SERIAL`), colors, Q8.8 motion macros |
| **Input** | NES Classic (clone) over I2C |
| **World** | One 320 px section in `PROGMEM`, tiled forever; gone-brick / used-`?` tracking |
| **Game** | Fixed-timestep physics, AABB solids, enemies, items, score / lives / death |
| **Display** | Per-column compose → `colBuf[]`, dirty rects, partial SPI writes, start-line pan |
| **UI** | Immediate-mode menus (Adafruit GFX font) + mode transitions |
| **Audio** | Timer1 CTC square wave on D9 + flash note phrases |

## Data model

### World

`ObjDef { x, y, type }` table (`WORLD[]` in flash). Object types: cloud, hill, block, question block, coin, pipe. Paint order = draw order (later entries cover earlier ones).

There is **no parallax**: hardware scroll moves the whole GRAM at once, so the backdrop is part of the world and repeats with it.

### Player

Fixed-point Q8.8 (`playerXq` / `playerYq` and velocities). Camera tracks the player with a left margin (`CAMERA_MARGIN = 40`).

### Column buffer

`uint16_t colBuf[128]` — one world column from sky down to dirt. `clipTop` / `clipBot` limit compositing work when only Mario’s band needs a repaint.

## Coordinate and orientation

- Game Y maps into a GRAM column index via `gramCol()`. `FLIP_Y` mirrors that mapping.
- `SCROLL_REVERSE` flips start-line math if scroll direction is wrong.
- `setup()` remaps the panel so GRAM row N lands on panel row N and the start-line offset stays a straightforward add.

## Game loop

`loop()` advances the audio sequencer, then runs a fixed simulation step of `STEP_MS` (33 ms ≈ 30 Hz), with a catch-up cap of 3 steps per pass:

1. `audioUpdate()` — non-blocking note scheduler (Timer1 toggles D9).
2. Read the pad → `updatePlayer` (move, jump edge, gravity, solids, floor, camera).
3. If any step ran → `render()`.
4. Once per second, print FPS and average pixels/frame over Serial (115200 baud).

## Rendering pipeline

`render()`:

1. **Cold start / large jump** (`!panelValid` or `|camera delta| ≥ 128`): paint all 128 columns at full height.
2. **Incremental**:
   - If the camera moved, paint only newly visible columns.
   - Union Mario’s previous and current AABB; repaint those columns in `[y0, y1]` (world first, then sprite, so old pixels clear in the same write).
3. `setStartLine(cameraX)` — hardware pan.
4. Cache `panelCam` and Mario’s column/row for the next dirty rect.

`paintColumn` = set clip → `composeColumn` → `composeRunner` → `pushColumn` (`setAddrWindow` + `writePixels`).

## Physics

- Instant left/right velocity (no acceleration); jump on A/B edge while grounded.
- Gravity with terminal fall velocity; solids resolved by minimum-overlap axis push (X and Y applied separately after each axis move).
- Solid types: blocks, question blocks, pipes. Ground is a floor clamp at `GROUND_Y`.
- Collision checks three adjacent 320 px world sections around the player.

## Memory and performance

- World data, Serial strings, and audio phrases live in `PROGMEM` / `F()`.
- One 256-byte column buffer — not a full framebuffer.
- Precomputed `circTab[12][12]` for cloud / hill / coin discs.
- Audio SRAM is a few bytes of sequencer state; the square wave is hardware PWM/CTC (no sample buffer).
- Bus time is dominated by dirty pixels, not geometry math.

## Building

Open `Super_Marduino_Bros/Super_Marduino_Bros.ino` in the Arduino IDE (or use `arduino-cli`). The IDE compiles every `.cpp` in that folder with the sketch. Requires:

- [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library)
- [Adafruit SSD1351](https://github.com/adafruit/Adafruit-SSD1351-library)

Select your board (e.g. Arduino Nano / Uno), upload. Set `DEBUG_SERIAL` to `1` in `Config.h` for FPS stats over Serial at 115200 baud (costs flash).
