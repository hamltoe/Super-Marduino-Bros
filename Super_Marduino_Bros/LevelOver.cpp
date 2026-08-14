#include "World.h"
#include <avr/pgmspace.h>

// World 1-1 — compact overworld (pipes omitted to save flash).
static const ObjDef OBJS[] PROGMEM = {
  {  15, 17, O_CLOUD},
  { 190, 31, O_CLOUD},
  { 520, 22, O_CLOUD},
  {   5,  0, O_HILL},
  { 300,  0, O_HILL2},
  { 640,  0, O_HILL},

  {  72, 59, O_BLOCK},
  {  87, 59, O_QBLOCK},
  { 102, 59, O_BLOCK},
  { 145, 70, O_COIN},

  { 220, 48, O_COIN},
  { 250, 59, O_QBLOCK},
  { 310, 59, O_BLOCK},
  { 325, 59, O_BLOCK},
  { 340, 59, O_QBLOCK},
  { 450, 55, O_COIN},
  { 520, 48, O_BLOCK},
  { 535, 48, O_QBLOCK},
  { 550, 48, O_BLOCK},
  { 670, 70, O_COIN},
  { 800, 48, O_BLOCK},
  { 815, 48, O_QBLOCK},
  { 830, 48, O_BLOCK},

  { 900, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 915, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 915, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  { 930, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  { 930, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  { 930, (uint8_t)(GROUND_Y - 45), O_BLOCK},
  { 960, (uint8_t)(GROUND_Y - 72), O_FLAG},
};

static const EnemyDef SPAWNS[] PROGMEM = {
  {120, 0, E_GOOMBA},
  {280, 0, E_GOOMBA},
  {450, 0, E_GOOMBA},
  {700, 0, E_GOOMBA},
};

const LevelDef LEVEL_OVER PROGMEM = {
  OBJS, SPAWNS, 0,
  1024,
  (uint8_t)(sizeof(OBJS) / sizeof(OBJS[0])),
  (uint8_t)(sizeof(SPAWNS) / sizeof(SPAWNS[0])),
  0,
  GROUND_Y,
  TH_OVER,
  0
};
