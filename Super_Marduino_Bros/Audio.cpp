/***************************************************
  Compact piezo sequencer — Timer1 CTC toggle on D9.

  Notes are packed {freq/8, ms} in PROGMEM (2 bytes each).
  No Arduino tone() — saves ~1 KB flash on ATmega328P.
 ****************************************************/
#include "Audio.h"
#include <avr/io.h>
#include <avr/pgmspace.h>

struct Note {
  uint8_t hz8;
  uint8_t ms;
};

#define N(hz, d) { (uint8_t)((hz) / 8), (uint8_t)(d) }

// Keep phrases short — flash is tight on the Nano.
static const Note PH_JUMP[] PROGMEM = { N(392, 30), N(523, 40), N(659, 45) };
static const Note PH_COIN[] PROGMEM = { N(988, 60), N(1319, 140) };
static const Note PH_STOMP[] PROGMEM = { N(220, 35), N(165, 45) };
static const Note PH_BUMP[] PROGMEM = { N(330, 40), N(247, 40) };
static const Note PH_BREAK[] PROGMEM = { N(392, 25), N(220, 40) };
static const Note PH_KICK[] PROGMEM = { N(440, 30), N(523, 40) };
static const Note PH_POWERUP[] PROGMEM = {
  N(523, 40), N(659, 40), N(784, 40), N(1047, 90)
};
static const Note PH_POWERDOWN[] PROGMEM = { N(784, 40), N(523, 40), N(392, 70) };
static const Note PH_PAUSE[] PROGMEM = { N(784, 50), N(523, 60) };
static const Note PH_BLIP[] PROGMEM = { N(880, 35), N(1175, 50) };
static const Note PH_DEATH[] PROGMEM = {
  N(523, 60), N(440, 70), N(330, 90), N(220, 160)
};
static const Note PH_GAMEOVER[] PROGMEM = {
  N(392, 100), N(330, 120), N(262, 160), N(196, 200)
};
static const Note PH_BGM[] PROGMEM = {
  N(523, 110), N(659, 110), N(784, 110), N(659, 110),
  N(698, 110), N(880, 110), N(784, 200),
  N(659, 110), N(523, 110), N(587, 110), N(523, 160), N(0, 50),
};

struct Phrase {
  const Note* notes;
  uint8_t len;
};

static const Phrase SFX_TABLE[SFX_COUNT] PROGMEM = {
  {PH_JUMP, sizeof(PH_JUMP) / sizeof(Note)},
  {PH_COIN, sizeof(PH_COIN) / sizeof(Note)},
  {PH_STOMP, sizeof(PH_STOMP) / sizeof(Note)},
  {PH_BUMP, sizeof(PH_BUMP) / sizeof(Note)},
  {PH_BREAK, sizeof(PH_BREAK) / sizeof(Note)},
  {PH_KICK, sizeof(PH_KICK) / sizeof(Note)},
  {PH_POWERUP, sizeof(PH_POWERUP) / sizeof(Note)},
  {PH_POWERDOWN, sizeof(PH_POWERDOWN) / sizeof(Note)},
  {PH_PAUSE, sizeof(PH_PAUSE) / sizeof(Note)},
  {PH_BLIP, sizeof(PH_BLIP) / sizeof(Note)},
  {PH_DEATH, sizeof(PH_DEATH) / sizeof(Note)},
  {PH_GAMEOVER, sizeof(PH_GAMEOVER) / sizeof(Note)},
};

static const Note* curNotes;
static uint8_t curLen, curIdx;
static uint16_t noteUntil;
static uint8_t flags;  // b0 sfx, b1 bgm, b2 paused
static uint8_t bgmIdx;
static uint16_t bgmUntil;

#define F_SFX   1
#define F_BGM   2
#define F_PAUSE 4

static void hwSilence() {
  TCCR1A = 0;
  TCCR1B = 0;
  digitalWrite(AUDIO_PIN, LOW);
}

static void hwTone(uint16_t hz) {
  if (!hz) { hwSilence(); return; }
  uint32_t ocr = ((F_CPU / 16UL) / (uint32_t)hz) - 1UL;
  if (ocr > 65535UL) ocr = 65535UL;
  OCR1A = (uint16_t)ocr;
  TCCR1A = _BV(COM1A0);
  TCCR1B = _BV(WGM12) | _BV(CS11);
}

static void beginPhrase(const Note* notes, uint8_t len) {
  curNotes = notes;
  curLen = len;
  curIdx = 0;
  flags |= F_SFX;
  if (!len) { hwSilence(); noteUntil = 0; return; }
  uint16_t now = (uint16_t)millis();
  hwTone((uint16_t)pgm_read_byte(&notes[0].hz8) << 3);
  noteUntil = now + pgm_read_byte(&notes[0].ms);
}

static void resumeBgm(uint16_t now) {
  if (!(flags & F_BGM) || (flags & F_PAUSE)) { hwSilence(); return; }
  const uint8_t len = sizeof(PH_BGM) / sizeof(Note);
  if (bgmIdx >= len) bgmIdx = 0;
  hwTone((uint16_t)pgm_read_byte(&PH_BGM[bgmIdx].hz8) << 3);
  bgmUntil = now + pgm_read_byte(&PH_BGM[bgmIdx].ms);
}

void audioInit() {
  pinMode(AUDIO_PIN, OUTPUT);
  hwSilence();
  flags = 0;
  curNotes = 0;
  curLen = curIdx = bgmIdx = 0;
  noteUntil = bgmUntil = 0;
  audioPlay(SFX_BLIP);
}

void audioPlay(uint8_t sfx) {
  if (sfx >= SFX_COUNT) return;
  Phrase ph;
  memcpy_P(&ph, &SFX_TABLE[sfx], sizeof(Phrase));
  beginPhrase(ph.notes, ph.len);
}

void audioStartBgm() {
  flags = (uint8_t)((flags & ~F_PAUSE) | F_BGM);
  bgmIdx = 0;
  bgmUntil = 0;
  if (!(flags & F_SFX)) resumeBgm((uint16_t)millis());
}

void audioStopBgm() {
  flags &= (uint8_t)~(F_BGM | F_PAUSE);
  bgmIdx = 0;
  if (!(flags & F_SFX)) hwSilence();
}

void audioSetPaused(bool paused) {
  if (paused) {
    flags |= F_PAUSE;
    if (!(flags & F_SFX)) hwSilence();
  } else {
    flags &= (uint8_t)~F_PAUSE;
    if ((flags & F_BGM) && !(flags & F_SFX)) resumeBgm((uint16_t)millis());
  }
}

void audioUpdate() {
  uint16_t now = (uint16_t)millis();

  if (flags & F_SFX) {
    if ((int16_t)(now - noteUntil) < 0) return;
    if (++curIdx >= curLen) {
      flags &= (uint8_t)~F_SFX;
      curNotes = 0;
      curLen = 0;
      if ((flags & F_BGM) && !(flags & F_PAUSE)) resumeBgm(now);
      else hwSilence();
      return;
    }
    hwTone((uint16_t)pgm_read_byte(&curNotes[curIdx].hz8) << 3);
    noteUntil = now + pgm_read_byte(&curNotes[curIdx].ms);
    return;
  }

  if (!(flags & F_BGM) || (flags & F_PAUSE)) return;
  if ((int16_t)(now - bgmUntil) < 0) return;
  if (++bgmIdx >= (uint8_t)(sizeof(PH_BGM) / sizeof(Note))) bgmIdx = 0;
  hwTone((uint16_t)pgm_read_byte(&PH_BGM[bgmIdx].hz8) << 3);
  bgmUntil = now + pgm_read_byte(&PH_BGM[bgmIdx].ms);
}
