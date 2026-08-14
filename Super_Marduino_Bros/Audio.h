/***************************************************
  Piezo on D9 via Timer1 CTC hardware toggle (OC1A).

  Pin D9 is fixed — it is the OC1A output, not a choice.
  Wiring: passive piezo D9 -> buzzer -> GND
          (or D9 -> 100 ohm -> 8 ohm speaker -> GND)

  Square wave is generated in hardware after each OCR1A write:
  no Timer1 ISR, no Arduino tone(), no per-cycle CPU cost.
  audioUpdate() only advances the note sequencer via millis().

  One voice: SFX preempt BGM, then BGM resumes.
  Call audioUpdate() every loop(); never block.
 ****************************************************/
#ifndef SUPER_MARDUINO_AUDIO_H
#define SUPER_MARDUINO_AUDIO_H

#include <Arduino.h>

// Documented for wiring/docs — hardware path is OC1A (must stay D9 / PB1).
#ifndef AUDIO_PIN
#define AUDIO_PIN 9
#endif

enum : uint8_t {
  SFX_JUMP = 0,
  SFX_COIN,
  SFX_STOMP,
  SFX_BUMP,
  SFX_BREAK,
  SFX_KICK,
  SFX_POWERUP,
  SFX_POWERDOWN,
  SFX_PAUSE,
  SFX_BLIP,      // menu select / confirm / boot
  SFX_DEATH,
  SFX_GAMEOVER,
  SFX_COUNT
};

void audioInit();
void audioUpdate();
void audioPlay(uint8_t sfx);
void audioStartBgm();
void audioStopBgm();
void audioSetPaused(bool paused);

#endif
