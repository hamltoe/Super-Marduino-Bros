/***************************************************
  Piezo on D9 via Timer1 CTC (not Arduino tone()).

  One voice: SFX preempt BGM, then BGM resumes.
  Call audioUpdate() every loop(); never block.
 ****************************************************/
#ifndef SUPER_MARDUINO_AUDIO_H
#define SUPER_MARDUINO_AUDIO_H

#include <Arduino.h>

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
