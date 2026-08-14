#include "World.h"
#include <avr/pgmspace.h>

// World 1-4 — short swim, cheep-cheeps at three heights.
static const ObjDef OBJS[] PROGMEM = {
  {  40, 48, O_COIN},
  {  56, 40, O_COIN},
  {  72, 48, O_COIN},
  { 140, 56, O_BLOCK},
  { 155, 56, O_BLOCK},
  { 200, 32, O_COIN},
  { 216, 32, O_COIN},
  { 280, 64, O_QBLOCK},
  { 320, 48, O_COIN},
  { 400, (uint8_t)(GROUND_Y - 72), O_FLAG},
};

static const EnemyDef SPAWNS[] PROGMEM = {
  { 90, 42, E_FISH},
  {180, 64, E_FISH},
  {300, 36, E_FISH},
};

const LevelDef LEVEL_WATER PROGMEM = {
  OBJS, SPAWNS, 0,
  512,
  (uint8_t)(sizeof(OBJS) / sizeof(OBJS[0])),
  (uint8_t)(sizeof(SPAWNS) / sizeof(SPAWNS[0])),
  0,
  GROUND_Y,
  TH_WATER,
  LF_SWIM
};
