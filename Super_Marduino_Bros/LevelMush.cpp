#include "World.h"
#include <avr/pgmspace.h>

// World 1-3 — athletic: mushroom caps + two moving beams. No floor.
static const ObjDef OBJS[] PROGMEM = {
  {   0, 72, O_PLAT},
  {  48, 56, O_COIN},
  {  56, 48, O_PLAT},
  { 200, 40, O_PLAT},
  { 210, 24, O_COIN},
  { 260, 72, O_PLAT},
  { 340, 48, O_PLAT},
  { 400, 72, O_PLAT},
  { 420,  0, O_FLAG},
};

static const EnemyDef SPAWNS[] PROGMEM = {
  {270, 72, E_GOOMBA},
};

static const BeamDef BEAMS[] PROGMEM = {
  {110, 190, 64, 28},
  {290, 370, 56, 28},
};

const LevelDef LEVEL_MUSH PROGMEM = {
  OBJS, SPAWNS, BEAMS,
  512,
  (uint8_t)(sizeof(OBJS) / sizeof(OBJS[0])),
  (uint8_t)(sizeof(SPAWNS) / sizeof(SPAWNS[0])),
  (uint8_t)(sizeof(BEAMS) / sizeof(BEAMS[0])),
  72,
  TH_MUSH,
  LF_PITS
};
