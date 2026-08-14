/***************************************************
  Compact piezo sequencer — Timer1 CTC toggle on OC1A (D9).

  Hardware toggles the pin on every compare match. No Timer1 ISR,
  no Arduino tone(), no CPU cost after the OCR1A write.

  Notes are packed {freq/8, ms} in PROGMEM (2 bytes each).
 ****************************************************/
#include "Audio.h"
#include "Config.h"
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>

extern uint8_t levelTheme;

// D9 == PB1 == OC1A — fixed by the COM1A0 hardware path.
#define SPEAKER_DDR   DDRB
#define SPEAKER_PORT  PORTB
#define SPEAKER_BIT   PB1

// Brief silence at the tail of long notes so repeated pitches separate.
static const uint8_t ARTICULATION_MS = 12;

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
// Overworld theme, 1-voice reduction of the project MIDI (120 BPM).
// Intro + A + A + B + B + C + C' (~34 s). C' ends like the intro, so
// the sequencer skips back to A (not the intro) on loop.
static const Note PH_BGM_OVER[] PROGMEM = {
  // Intro (~2 s)
  N(659, 124), N(659, 249), N(659, 124), N(0, 126),
  N(523, 124), N(659, 124), N(0, 126), N(784, 249),
  N(0, 250), N(392, 249), N(0, 250),
  // A + A (~8 s)
  N(523, 249),
  N(0, 126), N(392, 249), N(0, 126), N(330, 249),
  N(0, 126), N(440, 249), N(494, 249), N(466, 124),
  N(440, 124), N(0, 126), N(392, 166), N(659, 166),
  N(784, 166), N(880, 249), N(698, 124), N(784, 124),
  N(0, 126), N(659, 249), N(523, 124), N(587, 124),
  N(494, 124), N(0, 250), N(523, 249), N(0, 126),
  N(392, 249), N(0, 126), N(330, 249), N(0, 126),
  N(440, 249), N(494, 249), N(466, 124), N(440, 124),
  N(0, 126), N(392, 166), N(659, 166), N(784, 166),
  N(880, 249), N(698, 124), N(784, 124), N(0, 126),
  N(659, 249), N(523, 124), N(587, 124), N(494, 124),
  N(0, 250),
  // B + B (~16 s)
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(415, 124), N(440, 124), N(523, 124),
  N(0, 126), N(440, 124), N(523, 124), N(587, 124),
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(1047, 124), N(0, 126), N(1047, 124),
  N(1047, 124), N(0, 126), N(392, 124), N(0, 126),
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(415, 124), N(440, 124), N(523, 124),
  N(0, 126), N(440, 124), N(523, 124), N(587, 124),
  N(0, 250), N(622, 124), N(0, 250), N(587, 124),
  N(0, 250), N(523, 124), N(0, 250), N(392, 124),
  N(392, 124), N(0, 126), N(262, 124), N(0, 126),
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(415, 124), N(440, 124), N(523, 124),
  N(0, 126), N(440, 124), N(523, 124), N(587, 124),
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(1047, 124), N(0, 126), N(1047, 124),
  N(1047, 124), N(0, 126), N(392, 124), N(0, 126),
  N(262, 124), N(0, 126), N(784, 124), N(740, 124),
  N(698, 124), N(622, 124), N(0, 126), N(659, 124),
  N(0, 126), N(415, 124), N(440, 124), N(523, 124),
  N(0, 126), N(440, 124), N(523, 124), N(587, 124),
  N(0, 250), N(622, 124), N(0, 250), N(587, 124),
  N(0, 250), N(523, 124), N(0, 250), N(392, 124),
  N(392, 124), N(0, 126), N(262, 124), N(0, 126),
  // C + C' (~8 s). C' cadence is the intro figure, then loop to A.
  N(523, 124), N(523, 124), N(0, 126), N(523, 124),
  N(0, 126), N(523, 124), N(587, 124), N(0, 126),
  N(659, 124), N(523, 124), N(0, 126), N(440, 124),
  N(392, 124), N(0, 126), N(262, 124), N(0, 126),
  N(523, 124), N(523, 124), N(0, 126), N(523, 124),
  N(0, 126), N(523, 124), N(587, 124), N(659, 124),
  N(392, 124), N(0, 250), N(262, 124), N(0, 250),
  N(262, 124), N(0, 126), N(523, 124), N(523, 124),
  N(0, 126), N(523, 124), N(0, 126), N(523, 124),
  N(587, 124), N(0, 126), N(659, 124), N(523, 124),
  N(0, 126), N(440, 124), N(392, 124), N(0, 126),
  N(262, 124), N(0, 126), N(659, 124), N(659, 124),
  N(0, 126), N(659, 124), N(0, 126), N(523, 124),
  N(659, 124), N(0, 126), N(784, 249), N(0, 250),
  N(392, 249), N(0, 250),
};

// Underwater waltz, 1-voice reduction of the project MIDI (3/4, 160 BPM).
// Intro + A + B + C (~37 s). The opening scale restarts here, so loop to 0.
static const Note PH_BGM_WATER[] PROGMEM = {
  // Intro — rising scale (~4.5 s)
  N(294, 250), N(294, 62), N(0, 62), N(330, 94),
  N(0, 250), N(0, 31), N(370, 94), N(0, 250),
  N(0, 31), N(392, 94), N(0, 250), N(0, 31),
  N(440, 94), N(0, 250), N(0, 31), N(466, 94),
  N(0, 250), N(0, 31), N(494, 62), N(0, 125),
  N(494, 62), N(0, 125), N(494, 94), N(0, 250),
  N(0, 31), N(494, 94), N(0, 250), N(0, 31),
  N(494, 250), N(494, 250), N(494, 156), N(0, 94),
  N(294, 250), N(294, 62), N(0, 62),
  // A (~8.6 s)
  N(494, 250), N(494, 250), N(494, 250), N(494, 250),
  N(494, 31), N(0, 94), N(466, 250), N(466, 250),
  N(466, 250), N(466, 250), N(466, 31), N(0, 94),
  N(494, 250), N(494, 250), N(494, 250), N(494, 250),
  N(494, 31), N(0, 250), N(0, 31), N(294, 62),
  N(0, 125), N(330, 62), N(0, 125), N(370, 62),
  N(0, 125), N(392, 62), N(0, 125), N(440, 62),
  N(0, 125), N(494, 250), N(494, 250), N(494, 250),
  N(494, 250), N(494, 31), N(0, 94), N(466, 250),
  N(466, 250), N(466, 250), N(523, 250), N(523, 62),
  N(0, 62), N(494, 250), N(494, 250), N(494, 250),
  N(494, 250), N(494, 31), N(0, 250), N(0, 250),
  N(0, 250), N(0, 94), N(294, 94), N(0, 250),
  N(0, 31),
  // B (~9 s)
  N(440, 250), N(440, 250), N(440, 250), N(440, 250),
  N(440, 31), N(0, 94), N(415, 250), N(415, 250),
  N(415, 250), N(415, 250), N(415, 31), N(0, 94),
  N(440, 250), N(440, 250), N(440, 250), N(440, 250),
  N(440, 31), N(0, 250), N(0, 31), N(277, 62),
  N(0, 125), N(294, 62), N(0, 125), N(330, 62),
  N(0, 125), N(370, 62), N(0, 125), N(392, 62),
  N(0, 125), N(440, 250), N(440, 250), N(440, 250),
  N(440, 250), N(440, 31), N(0, 94), N(294, 250),
  N(294, 250), N(294, 250), N(523, 250), N(523, 62),
  N(0, 62), N(494, 250), N(494, 250), N(494, 250),
  N(494, 250), N(494, 31), N(0, 250), N(0, 250),
  N(0, 250), N(0, 94), N(392, 94), N(0, 250),
  N(0, 31),
  // C (~15 s). Cadence lands on G, then the opening scale repeats.
  N(587, 250), N(587, 250), N(587, 250), N(587, 250),
  N(587, 31), N(0, 94), N(587, 250), N(587, 250),
  N(587, 250), N(587, 250), N(587, 31), N(0, 94),
  N(587, 250), N(587, 250), N(587, 250), N(587, 250),
  N(587, 31), N(0, 94), N(587, 94), N(0, 250),
  N(0, 31), N(659, 94), N(0, 250), N(0, 219),
  N(587, 62), N(0, 125), N(523, 250), N(523, 250),
  N(523, 250), N(523, 250), N(523, 31), N(0, 94),
  N(523, 250), N(523, 250), N(523, 250), N(523, 250),
  N(523, 31), N(0, 94), N(523, 250), N(523, 250),
  N(523, 250), N(523, 250), N(523, 31), N(0, 94),
  N(523, 94), N(0, 250), N(0, 31), N(587, 94),
  N(0, 250), N(0, 219), N(523, 62), N(0, 125),
  N(494, 250), N(494, 250), N(494, 250), N(494, 250),
  N(494, 31), N(0, 94), N(294, 94), N(0, 250),
  N(0, 31), N(392, 94), N(0, 250), N(0, 31),
  N(523, 94), N(0, 250), N(0, 31), N(494, 62),
  N(0, 125), N(494, 62), N(0, 125), N(494, 62),
  N(0, 250), N(0, 250), N(370, 62), N(0, 125),
  N(392, 250), N(392, 250), N(392, 250), N(392, 250),
  N(392, 31), N(0, 94), N(392, 250), N(392, 250),
  N(392, 156), N(0, 250), N(0, 219),
};

static const uint8_t BGM_OVER_LOOP_IDX = 11;

static const Note* bgmNotes = PH_BGM_OVER;
static uint8_t bgmLen = sizeof(PH_BGM_OVER) / sizeof(Note);
static uint8_t bgmLoopIdx = BGM_OVER_LOOP_IDX;

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
static uint16_t noteStartMs;
static uint16_t noteLenMs;
static uint8_t flags;  // b0 sfx, b1 bgm, b2 paused, b3 tail silenced
static uint8_t bgmIdx;
static uint16_t bgmStartMs;
static uint16_t bgmLenMs;

#define F_SFX   1
#define F_BGM   2
#define F_PAUSE 4
#define F_TAIL  8

// Prescaler /8: OCR1A = F_CPU / (2 * 8 * hz) - 1 = 1000000 / hz - 1
// Usable range: ~16 Hz (OCR 62499) to ~8 kHz (OCR 124).
static void toneStop() {
  TCCR1A = 0;                          // disconnect OC1A from the pin
  TCCR1B = 0;                          // stop the clock
  TIMSK1 = 0;                          // no Timer1 interrupts
  SPEAKER_PORT &= ~_BV(SPEAKER_BIT);   // park low: no DC through the coil
}

static void toneStart(uint16_t hz) {
  if (hz < 16) { toneStop(); return; }

  uint16_t top = (uint16_t)(1000000UL / hz) - 1;

  uint8_t sreg = SREG;
  cli();                               // 16-bit register writes must be atomic
  OCR1A = top;
  if (TCNT1 > top) TCNT1 = 0;          // else counter races to 0xFFFF — glitch
  TCCR1A = _BV(COM1A0);                // toggle OC1A on compare match
  TCCR1B = _BV(WGM12) | _BV(CS11);     // CTC (TOP = OCR1A), prescaler /8
  TIMSK1 = 0;
  SREG = sreg;
}

static uint16_t noteHz(const Note* notes, uint8_t i) {
  return (uint16_t)pgm_read_byte(&notes[i].hz8) << 3;
}

static uint8_t noteMs(const Note* notes, uint8_t i) {
  return pgm_read_byte(&notes[i].ms);
}

static void startSfxNote(uint8_t i, uint16_t now) {
  curIdx = i;
  noteLenMs = noteMs(curNotes, i);
  noteStartMs = now;
  flags &= (uint8_t)~F_TAIL;
  toneStart(noteHz(curNotes, i));
}

static void startBgmNote(uint8_t i, uint16_t now) {
  if (i >= bgmLen) i = bgmLoopIdx;
  bgmIdx = i;
  bgmLenMs = noteMs(bgmNotes, i);
  bgmStartMs = now;
  flags &= (uint8_t)~F_TAIL;
  toneStart(noteHz(bgmNotes, i));
}

static void beginPhrase(const Note* notes, uint8_t len) {
  curNotes = notes;
  curLen = len;
  flags = (uint8_t)((flags | F_SFX) & (uint8_t)~F_TAIL);
  if (!len) {
    toneStop();
    noteLenMs = 0;
    return;
  }
  startSfxNote(0, (uint16_t)millis());
}

static void resumeBgm(uint16_t now) {
  if (!(flags & F_BGM) || (flags & F_PAUSE)) { toneStop(); return; }
  startBgmNote(bgmIdx, now);
}

// Silence the tail of a note once, when the note is long enough that a gap helps.
// BGM skips that gap when the next event is a rest (gap is already encoded) or
// the same pitch (a long waltz note split across uint8 durations).
static bool bgmKeepsTone() {
  uint8_t next = bgmIdx + 1;
  if (next >= bgmLen) next = bgmLoopIdx;
  uint16_t cur = noteHz(bgmNotes, bgmIdx);
  if (cur < 16) return false;
  uint16_t nxt = noteHz(bgmNotes, next);
  return nxt < 16 || nxt == cur;
}

static bool maybeArticulate(uint16_t elapsed, uint16_t lenMs) {
  if (flags & F_TAIL) return true;
  if (lenMs <= (uint16_t)(ARTICULATION_MS * 2)) return false;
  if (elapsed < lenMs - ARTICULATION_MS) return false;
  if (!(flags & F_SFX) && bgmKeepsTone()) return false;
  toneStop();
  flags |= F_TAIL;
  return true;
}

void audioInit() {
  SPEAKER_DDR |= _BV(SPEAKER_BIT);   // D9 as output — required for OC1A
  toneStop();
  flags = 0;
  curNotes = 0;
  curLen = curIdx = bgmIdx = 0;
  noteStartMs = noteLenMs = bgmStartMs = bgmLenMs = 0;
}

void audioPlay(uint8_t sfx) {
  if (sfx >= SFX_COUNT) return;
  Phrase ph;
  memcpy_P(&ph, &SFX_TABLE[sfx], sizeof(Phrase));
  beginPhrase(ph.notes, ph.len);
}

void audioStartBgm() {
  if (levelTheme == TH_WATER) {
    bgmNotes = PH_BGM_WATER;
    bgmLen = (uint8_t)(sizeof(PH_BGM_WATER) / sizeof(Note));
    bgmLoopIdx = 0;
  } else {
    bgmNotes = PH_BGM_OVER;
    bgmLen = (uint8_t)(sizeof(PH_BGM_OVER) / sizeof(Note));
    bgmLoopIdx = BGM_OVER_LOOP_IDX;
  }
  flags = (uint8_t)((flags & (uint8_t)~(F_PAUSE | F_TAIL)) | F_BGM);
  bgmIdx = 0;
  bgmLenMs = 0;
  if (!(flags & F_SFX)) resumeBgm((uint16_t)millis());
}

void audioStopBgm() {
  flags &= (uint8_t)~(F_BGM | F_PAUSE | F_TAIL);
  bgmIdx = 0;
  if (!(flags & F_SFX)) toneStop();
}

void audioSetPaused(bool paused) {
  if (paused) {
    flags |= F_PAUSE;
    if (!(flags & F_SFX)) toneStop();
  } else {
    flags &= (uint8_t)~F_PAUSE;
    if ((flags & F_BGM) && !(flags & F_SFX)) resumeBgm((uint16_t)millis());
  }
}

void audioUpdate() {
  uint16_t now = (uint16_t)millis();

  if (flags & F_SFX) {
    uint16_t elapsed = now - noteStartMs;
    maybeArticulate(elapsed, noteLenMs);
    if ((int16_t)(elapsed - noteLenMs) < 0) return;

    if (++curIdx >= curLen) {
      flags &= (uint8_t)~(F_SFX | F_TAIL);
      curNotes = 0;
      curLen = 0;
      if ((flags & F_BGM) && !(flags & F_PAUSE)) resumeBgm(now);
      else toneStop();
      return;
    }
    startSfxNote(curIdx, now);
    return;
  }

  if (!(flags & F_BGM) || (flags & F_PAUSE)) return;

  uint16_t elapsed = now - bgmStartMs;
  maybeArticulate(elapsed, bgmLenMs);
  if ((int16_t)(elapsed - bgmLenMs) < 0) return;

  uint8_t next = bgmIdx + 1;
  if (next >= bgmLen) next = bgmLoopIdx;
  startBgmNote(next, now);
}
