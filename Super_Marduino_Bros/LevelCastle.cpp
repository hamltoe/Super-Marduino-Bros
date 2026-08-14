#include "World.h"
#include <avr/pgmspace.h>

// World 1-5 — lava pits, stone bridge, Bowser, axe (drawn as the flag).
static const ObjDef OBJS[] PROGMEM = {
  {   0, 88, O_PLAT},
  {  56, 72, O_PLAT},
  {  80, 40, O_COIN},
  { 120, 73, O_BLOCK},
  { 120, 88, O_PLAT},
  { 160, 88, O_PLAT},
  { 200, 88, O_PLAT},
  { 240, 88, O_PLAT},
  { 280, 88, O_PLAT},
  { 300, 73, O_BLOCK},
  { 320, 88, O_PLAT},
  { 348, 76, O_FLAG},
};

static const EnemyDef SPAWNS[] PROGMEM = {
  {220, 88, E_BOWSER},
};

const LevelDef LEVEL_CASTLE PROGMEM = {
  OBJS, SPAWNS, 0,
  448,
  (uint8_t)(sizeof(OBJS) / sizeof(OBJS[0])),
  (uint8_t)(sizeof(SPAWNS) / sizeof(SPAWNS[0])),
  0,
  88,
  TH_CASTLE,
  (uint8_t)(LF_PITS | LF_LAVA)
};
