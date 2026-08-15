/***************************************************
  Compact piezo sequencer — Timer1 CTC toggle on OC1A (D9).

  Hardware toggles the pin on every compare match. No Timer1 ISR,
  no Arduino tone(), no CPU cost after the OCR1A write.

  Unique {freq/8, ms} pairs live in NOTE_DICT (2 bytes each).
  Phrases store 1-byte indices instead of repeating those pairs.
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
#define I(hz, d) NI_##hz##_##d
#define PH_LEN(a) ((uint8_t)sizeof(a))

// Unique {freq/8, ms} pairs. Add a new X() here if a phrase uses a new combo;
// I(hz, ms) then indexes this table. Phrases store those 1-byte indices.
#define NOTE_PAIRS \
  X(0, 31) \
  X(0, 45) \
  X(0, 62) \
  X(0, 91) \
  X(0, 94) \
  X(0, 114) \
  X(0, 125) \
  X(0, 126) \
  X(0, 136) \
  X(0, 219) \
  X(0, 250) \
  X(147, 136) \
  X(156, 136) \
  X(165, 45) \
  X(175, 136) \
  X(196, 136) \
  X(196, 200) \
  X(208, 136) \
  X(220, 35) \
  X(220, 40) \
  X(220, 136) \
  X(220, 160) \
  X(233, 136) \
  X(247, 40) \
  X(247, 136) \
  X(262, 91) \
  X(262, 124) \
  X(262, 136) \
  X(262, 160) \
  X(277, 62) \
  X(277, 91) \
  X(277, 136) \
  X(294, 62) \
  X(294, 91) \
  X(294, 94) \
  X(294, 136) \
  X(294, 167) \
  X(294, 250) \
  X(311, 91) \
  X(311, 136) \
  X(312, 83) \
  X(312, 167) \
  X(312, 250) \
  X(330, 40) \
  X(330, 62) \
  X(330, 90) \
  X(330, 91) \
  X(330, 94) \
  X(330, 120) \
  X(330, 167) \
  X(330, 249) \
  X(330, 250) \
  X(349, 91) \
  X(349, 136) \
  X(350, 83) \
  X(350, 250) \
  X(370, 62) \
  X(370, 91) \
  X(370, 94) \
  X(370, 167) \
  X(370, 250) \
  X(392, 25) \
  X(392, 30) \
  X(392, 31) \
  X(392, 62) \
  X(392, 70) \
  X(392, 94) \
  X(392, 100) \
  X(392, 124) \
  X(392, 156) \
  X(392, 166) \
  X(392, 249) \
  X(392, 250) \
  X(415, 31) \
  X(415, 124) \
  X(415, 136) \
  X(415, 250) \
  X(440, 30) \
  X(440, 31) \
  X(440, 62) \
  X(440, 70) \
  X(440, 91) \
  X(440, 94) \
  X(440, 124) \
  X(440, 136) \
  X(440, 167) \
  X(440, 249) \
  X(440, 250) \
  X(466, 31) \
  X(466, 91) \
  X(466, 94) \
  X(466, 124) \
  X(466, 136) \
  X(466, 167) \
  X(466, 250) \
  X(494, 31) \
  X(494, 62) \
  X(494, 94) \
  X(494, 124) \
  X(494, 156) \
  X(494, 249) \
  X(494, 250) \
  X(523, 31) \
  X(523, 40) \
  X(523, 60) \
  X(523, 62) \
  X(523, 94) \
  X(523, 124) \
  X(523, 136) \
  X(523, 249) \
  X(523, 250) \
  X(587, 31) \
  X(587, 62) \
  X(587, 94) \
  X(587, 124) \
  X(587, 250) \
  X(622, 124) \
  X(659, 40) \
  X(659, 45) \
  X(659, 94) \
  X(659, 124) \
  X(659, 166) \
  X(659, 249) \
  X(698, 124) \
  X(740, 124) \
  X(784, 40) \
  X(784, 50) \
  X(784, 124) \
  X(784, 166) \
  X(784, 249) \
  X(880, 35) \
  X(880, 249) \
  X(988, 60) \
  X(1047, 90) \
  X(1047, 124) \
  X(1175, 50) \
  X(1319, 140)

enum : uint8_t {
#define X(hz, d) NI_##hz##_##d,
  NOTE_PAIRS
#undef X
};

static const Note NOTE_DICT[] PROGMEM = {
#define X(hz, d) N(hz, d),
  NOTE_PAIRS
#undef X
};


// Keep phrases short — flash is tight on the Nano.
static const uint8_t PH_JUMP[] PROGMEM = { I(392, 30), I(523, 40), I(659, 45) };
static const uint8_t PH_COIN[] PROGMEM = { I(988, 60), I(1319, 140) };
static const uint8_t PH_STOMP[] PROGMEM = { I(220, 35), I(165, 45) };
static const uint8_t PH_BUMP[] PROGMEM = { I(330, 40), I(247, 40) };
static const uint8_t PH_BREAK[] PROGMEM = { I(392, 25), I(220, 40) };
static const uint8_t PH_KICK[] PROGMEM = { I(440, 30), I(523, 40) };
static const uint8_t PH_POWERUP[] PROGMEM = {
  I(523, 40), I(659, 40), I(784, 40), I(1047, 90)
};
static const uint8_t PH_POWERDOWN[] PROGMEM = { I(784, 40), I(523, 40), I(392, 70) };
static const uint8_t PH_PAUSE[] PROGMEM = { I(784, 50), I(523, 60) };
static const uint8_t PH_BLIP[] PROGMEM = { I(880, 35), I(1175, 50) };
static const uint8_t PH_DEATH[] PROGMEM = {
  I(523, 60), I(440, 70), I(330, 90), I(220, 160)
};
static const uint8_t PH_GAMEOVER[] PROGMEM = {
  I(392, 100), I(330, 120), I(262, 160), I(196, 200)
};
// Overworld theme, 1-voice reduction of the project MIDI (120 BPM).
// Intro + A + A + B + B + C + C' (~34 s). C' ends like the intro, so
// the sequencer skips back to A (not the intro) on loop.
static const uint8_t PH_BGM_OVER[] PROGMEM = {
  // Intro (~2 s)
  I(659, 124), I(659, 249), I(659, 124), I(0, 126),
  I(523, 124), I(659, 124), I(0, 126), I(784, 249),
  I(0, 250), I(392, 249), I(0, 250),
  // A + A (~8 s)
  I(523, 249),
  I(0, 126), I(392, 249), I(0, 126), I(330, 249),
  I(0, 126), I(440, 249), I(494, 249), I(466, 124),
  I(440, 124), I(0, 126), I(392, 166), I(659, 166),
  I(784, 166), I(880, 249), I(698, 124), I(784, 124),
  I(0, 126), I(659, 249), I(523, 124), I(587, 124),
  I(494, 124), I(0, 250), I(523, 249), I(0, 126),
  I(392, 249), I(0, 126), I(330, 249), I(0, 126),
  I(440, 249), I(494, 249), I(466, 124), I(440, 124),
  I(0, 126), I(392, 166), I(659, 166), I(784, 166),
  I(880, 249), I(698, 124), I(784, 124), I(0, 126),
  I(659, 249), I(523, 124), I(587, 124), I(494, 124),
  I(0, 250),
  // B + B (~16 s)
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(415, 124), I(440, 124), I(523, 124),
  I(0, 126), I(440, 124), I(523, 124), I(587, 124),
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(1047, 124), I(0, 126), I(1047, 124),
  I(1047, 124), I(0, 126), I(392, 124), I(0, 126),
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(415, 124), I(440, 124), I(523, 124),
  I(0, 126), I(440, 124), I(523, 124), I(587, 124),
  I(0, 250), I(622, 124), I(0, 250), I(587, 124),
  I(0, 250), I(523, 124), I(0, 250), I(392, 124),
  I(392, 124), I(0, 126), I(262, 124), I(0, 126),
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(415, 124), I(440, 124), I(523, 124),
  I(0, 126), I(440, 124), I(523, 124), I(587, 124),
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(1047, 124), I(0, 126), I(1047, 124),
  I(1047, 124), I(0, 126), I(392, 124), I(0, 126),
  I(262, 124), I(0, 126), I(784, 124), I(740, 124),
  I(698, 124), I(622, 124), I(0, 126), I(659, 124),
  I(0, 126), I(415, 124), I(440, 124), I(523, 124),
  I(0, 126), I(440, 124), I(523, 124), I(587, 124),
  I(0, 250), I(622, 124), I(0, 250), I(587, 124),
  I(0, 250), I(523, 124), I(0, 250), I(392, 124),
  I(392, 124), I(0, 126), I(262, 124), I(0, 126),
  // C + C' (~8 s). C' cadence is the intro figure, then loop to A.
  I(523, 124), I(523, 124), I(0, 126), I(523, 124),
  I(0, 126), I(523, 124), I(587, 124), I(0, 126),
  I(659, 124), I(523, 124), I(0, 126), I(440, 124),
  I(392, 124), I(0, 126), I(262, 124), I(0, 126),
  I(523, 124), I(523, 124), I(0, 126), I(523, 124),
  I(0, 126), I(523, 124), I(587, 124), I(659, 124),
  I(392, 124), I(0, 250), I(262, 124), I(0, 250),
  I(262, 124), I(0, 126), I(523, 124), I(523, 124),
  I(0, 126), I(523, 124), I(0, 126), I(523, 124),
  I(587, 124), I(0, 126), I(659, 124), I(523, 124),
  I(0, 126), I(440, 124), I(392, 124), I(0, 126),
  I(262, 124), I(0, 126), I(659, 124), I(659, 124),
  I(0, 126), I(659, 124), I(0, 126), I(523, 124),
  I(659, 124), I(0, 126), I(784, 249), I(0, 250),
  I(392, 249), I(0, 250),
};

// Underwater waltz, 1-voice reduction of the project MIDI (3/4, 160 BPM).
// Intro + A + B + C (~37 s). The opening scale restarts here, so loop to 0.
static const uint8_t PH_BGM_WATER[] PROGMEM = {
  // Intro — rising scale (~4.5 s)
  I(294, 250), I(294, 62), I(0, 62), I(330, 94),
  I(0, 250), I(0, 31), I(370, 94), I(0, 250),
  I(0, 31), I(392, 94), I(0, 250), I(0, 31),
  I(440, 94), I(0, 250), I(0, 31), I(466, 94),
  I(0, 250), I(0, 31), I(494, 62), I(0, 125),
  I(494, 62), I(0, 125), I(494, 94), I(0, 250),
  I(0, 31), I(494, 94), I(0, 250), I(0, 31),
  I(494, 250), I(494, 250), I(494, 156), I(0, 94),
  I(294, 250), I(294, 62), I(0, 62),
  // A (~8.6 s)
  I(494, 250), I(494, 250), I(494, 250), I(494, 250),
  I(494, 31), I(0, 94), I(466, 250), I(466, 250),
  I(466, 250), I(466, 250), I(466, 31), I(0, 94),
  I(494, 250), I(494, 250), I(494, 250), I(494, 250),
  I(494, 31), I(0, 250), I(0, 31), I(294, 62),
  I(0, 125), I(330, 62), I(0, 125), I(370, 62),
  I(0, 125), I(392, 62), I(0, 125), I(440, 62),
  I(0, 125), I(494, 250), I(494, 250), I(494, 250),
  I(494, 250), I(494, 31), I(0, 94), I(466, 250),
  I(466, 250), I(466, 250), I(523, 250), I(523, 62),
  I(0, 62), I(494, 250), I(494, 250), I(494, 250),
  I(494, 250), I(494, 31), I(0, 250), I(0, 250),
  I(0, 250), I(0, 94), I(294, 94), I(0, 250),
  I(0, 31),
  // B (~9 s)
  I(440, 250), I(440, 250), I(440, 250), I(440, 250),
  I(440, 31), I(0, 94), I(415, 250), I(415, 250),
  I(415, 250), I(415, 250), I(415, 31), I(0, 94),
  I(440, 250), I(440, 250), I(440, 250), I(440, 250),
  I(440, 31), I(0, 250), I(0, 31), I(277, 62),
  I(0, 125), I(294, 62), I(0, 125), I(330, 62),
  I(0, 125), I(370, 62), I(0, 125), I(392, 62),
  I(0, 125), I(440, 250), I(440, 250), I(440, 250),
  I(440, 250), I(440, 31), I(0, 94), I(294, 250),
  I(294, 250), I(294, 250), I(523, 250), I(523, 62),
  I(0, 62), I(494, 250), I(494, 250), I(494, 250),
  I(494, 250), I(494, 31), I(0, 250), I(0, 250),
  I(0, 250), I(0, 94), I(392, 94), I(0, 250),
  I(0, 31),
  // C (~15 s). Cadence lands on G, then the opening scale repeats.
  I(587, 250), I(587, 250), I(587, 250), I(587, 250),
  I(587, 31), I(0, 94), I(587, 250), I(587, 250),
  I(587, 250), I(587, 250), I(587, 31), I(0, 94),
  I(587, 250), I(587, 250), I(587, 250), I(587, 250),
  I(587, 31), I(0, 94), I(587, 94), I(0, 250),
  I(0, 31), I(659, 94), I(0, 250), I(0, 219),
  I(587, 62), I(0, 125), I(523, 250), I(523, 250),
  I(523, 250), I(523, 250), I(523, 31), I(0, 94),
  I(523, 250), I(523, 250), I(523, 250), I(523, 250),
  I(523, 31), I(0, 94), I(523, 250), I(523, 250),
  I(523, 250), I(523, 250), I(523, 31), I(0, 94),
  I(523, 94), I(0, 250), I(0, 31), I(587, 94),
  I(0, 250), I(0, 219), I(523, 62), I(0, 125),
  I(494, 250), I(494, 250), I(494, 250), I(494, 250),
  I(494, 31), I(0, 94), I(294, 94), I(0, 250),
  I(0, 31), I(392, 94), I(0, 250), I(0, 31),
  I(523, 94), I(0, 250), I(0, 31), I(494, 62),
  I(0, 125), I(494, 62), I(0, 125), I(494, 62),
  I(0, 250), I(0, 250), I(370, 62), I(0, 125),
  I(392, 250), I(392, 250), I(392, 250), I(392, 250),
  I(392, 31), I(0, 94), I(392, 250), I(392, 250),
  I(392, 156), I(0, 250), I(0, 219),
};

// Castle bass ostinato, left-hand only (90 BPM), +1 octave so the piezo
// can speak it. The MIDI's right-hand 16th figures are dropped. One 8 s
// cycle; the file is four repeats.
static const uint8_t PH_BGM_CASTLE[] PROGMEM = {
  I(312, 250), I(312, 250), I(312, 250), I(312, 250),
  I(312, 250), I(312, 83), I(294, 250), I(294, 250),
  I(294, 167), I(370, 250), I(370, 250), I(370, 167),
  I(350, 250), I(350, 250), I(350, 250), I(350, 250),
  I(350, 250), I(350, 83), I(330, 250), I(330, 250),
  I(330, 167), I(466, 250), I(466, 250), I(466, 167),
  I(440, 250), I(440, 250), I(440, 167), I(330, 250),
  I(330, 250), I(330, 167), I(312, 250), I(312, 250),
  I(312, 167), I(330, 250), I(330, 250), I(330, 167),
};

// Underground theme (110 BPM), +1 octave. MIDI lead-in rest omitted.
static const uint8_t PH_BGM_UNDER[] PROGMEM = {
  I(262, 136), I(523, 136), I(220, 136), I(440, 136),
  I(233, 136), I(466, 136), I(0, 250), I(0, 250),
  I(0, 250), I(0, 250), I(0, 250), I(0, 114),
  I(262, 136), I(523, 136), I(220, 136), I(440, 136),
  I(233, 136), I(466, 136), I(0, 250), I(0, 250),
  I(0, 250), I(0, 250), I(0, 250), I(0, 114),
  I(175, 136), I(349, 136), I(147, 136), I(294, 136),
  I(156, 136), I(311, 136), I(0, 250), I(0, 250),
  I(0, 250), I(0, 250), I(0, 250), I(0, 114),
  I(175, 136), I(349, 136), I(147, 136), I(294, 136),
  I(156, 136), I(311, 136), I(0, 250), I(0, 250),
  I(0, 250), I(0, 250), I(0, 91), I(311, 91),
  I(294, 91), I(277, 91), I(262, 136), I(0, 136),
  I(311, 136), I(0, 136), I(294, 136), I(0, 136),
  I(208, 136), I(0, 136), I(196, 136), I(0, 136),
  I(277, 136), I(0, 136), I(262, 91), I(370, 91),
  I(349, 91), I(330, 91), I(466, 91), I(440, 91),
  I(415, 136), I(0, 45), I(311, 136), I(0, 45),
  I(247, 136), I(0, 45), I(233, 136), I(0, 45),
  I(220, 136), I(0, 45), I(208, 136),
};

static const uint8_t BGM_OVER_LOOP_IDX = 11;

static const uint8_t* bgmNotes = PH_BGM_OVER;
static uint8_t bgmLen = PH_LEN(PH_BGM_OVER);
static uint8_t bgmLoopIdx = BGM_OVER_LOOP_IDX;

struct Phrase {
  const uint8_t* notes;
  uint8_t len;
};

static const Phrase SFX_TABLE[SFX_COUNT] PROGMEM = {
  {PH_JUMP, PH_LEN(PH_JUMP)},
  {PH_COIN, PH_LEN(PH_COIN)},
  {PH_STOMP, PH_LEN(PH_STOMP)},
  {PH_BUMP, PH_LEN(PH_BUMP)},
  {PH_BREAK, PH_LEN(PH_BREAK)},
  {PH_KICK, PH_LEN(PH_KICK)},
  {PH_POWERUP, PH_LEN(PH_POWERUP)},
  {PH_POWERDOWN, PH_LEN(PH_POWERDOWN)},
  {PH_PAUSE, PH_LEN(PH_PAUSE)},
  {PH_BLIP, PH_LEN(PH_BLIP)},
  {PH_DEATH, PH_LEN(PH_DEATH)},
  {PH_GAMEOVER, PH_LEN(PH_GAMEOVER)},
};

static const uint8_t* curNotes;
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

static inline uint8_t dictId(const uint8_t* seq, uint8_t i) {
  return pgm_read_byte(&seq[i]);
}

static void startSfxNote(uint8_t i, uint16_t now) {
  uint8_t id = dictId(curNotes, i);
  curIdx = i;
  noteLenMs = pgm_read_byte(&NOTE_DICT[id].ms);
  noteStartMs = now;
  flags &= (uint8_t)~F_TAIL;
  toneStart((uint16_t)pgm_read_byte(&NOTE_DICT[id].hz8) << 3);
}

static void startBgmNote(uint8_t i, uint16_t now) {
  if (i >= bgmLen) i = bgmLoopIdx;
  uint8_t id = dictId(bgmNotes, i);
  bgmIdx = i;
  bgmLenMs = pgm_read_byte(&NOTE_DICT[id].ms);
  bgmStartMs = now;
  flags &= (uint8_t)~F_TAIL;
  toneStart((uint16_t)pgm_read_byte(&NOTE_DICT[id].hz8) << 3);
}

static void beginPhrase(const uint8_t* notes, uint8_t len) {
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
  // hz = hz8 << 3, so hz < 16 means a rest (hz8 == 0).
  uint8_t cur = pgm_read_byte(&NOTE_DICT[dictId(bgmNotes, bgmIdx)].hz8);
  if (!cur) return false;
  uint8_t nxt = pgm_read_byte(&NOTE_DICT[dictId(bgmNotes, next)].hz8);
  return !nxt || nxt == cur;
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
    bgmLen = PH_LEN(PH_BGM_WATER);
    bgmLoopIdx = 0;
  } else if (levelTheme == TH_CASTLE) {
    bgmNotes = PH_BGM_CASTLE;
    bgmLen = PH_LEN(PH_BGM_CASTLE);
    bgmLoopIdx = 0;
  } else if (levelTheme == TH_UNDER) {
    bgmNotes = PH_BGM_UNDER;
    bgmLen = PH_LEN(PH_BGM_UNDER);
    bgmLoopIdx = 0;
  } else {
    bgmNotes = PH_BGM_OVER;
    bgmLen = PH_LEN(PH_BGM_OVER);
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
