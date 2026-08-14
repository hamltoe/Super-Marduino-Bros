#include "World.h"
#include <avr/pgmspace.h>

// World 1-2 — short underworld, dark sky, blue bricks.
static const ObjDef OBJS[] PROGMEM = {
  {  40, 48, O_BLOCK},
  {  55, 48, O_QBLOCK},
  {  70, 48, O_BLOCK},
  {  90, 70, O_COIN},
  { 120, 59, O_BLOCK},

  { 180, 40, O_BLOCK},
  { 195, 40, O_BLOCK},
  { 210, 40, O_BLOCK},
  { 195, 24, O_COIN},

  { 260, 56, O_BLOCK},
  { 275, 56, O_QBLOCK},
  { 290, 56, O_BLOCK},
  { 320, 70, O_COIN},

  { 360, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 375, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 375, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  { 390, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 390, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  { 390, (uint8_t)(GROUND_Y - 45), O_BLOCK},
  { 420, (uint8_t)(GROUND_Y - 72), O_FLAG},
};

static const EnemyDef SPAWNS[] PROGMEM = {
  {100, 0, E_GOOMBA},
  {230, 0, E_GOOMBA},
};

const LevelDef LEVEL_UNDER PROGMEM = {
  OBJS, SPAWNS, 0,
  512,
  (uint8_t)(sizeof(OBJS) / sizeof(OBJS[0])),
  (uint8_t)(sizeof(SPAWNS) / sizeof(SPAWNS[0])),
  0,
  GROUND_Y,
  TH_UNDER,
  0
};
