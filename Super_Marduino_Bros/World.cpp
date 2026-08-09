#include "World.h"
#include <avr/pgmspace.h>

// Finite World 1-1 (LEVEL_W px). No section wrap — ends at the flag.
const ObjDef WORLD[] PROGMEM = {
  // backdrop
  {  15, 17, O_CLOUD},
  { 190, 31, O_CLOUD},
  { 520, 22, O_CLOUD},
  { 900, 28, O_CLOUD},
  {   5,  0, O_HILL},
  { 300,  0, O_HILL2},
  { 640,  0, O_HILL},
  { 980,  0, O_HILL2},

  // opening
  {  72, 59, O_BLOCK},
  {  87, 59, O_QBLOCK},
  { 102, 59, O_BLOCK},
  { 145, 70, O_COIN},
  { 166, (uint8_t)(GROUND_Y - 34), O_PIPE},

  // mid
  { 220, 48, O_COIN},
  { 250, 59, O_QBLOCK},
  { 310, 59, O_BLOCK},
  { 325, 59, O_BLOCK},
  { 340, 59, O_QBLOCK},
  { 390, (uint8_t)(GROUND_Y - 34), O_PIPE},
  { 450, 55, O_COIN},
  { 520, 48, O_BLOCK},
  { 535, 48, O_QBLOCK},
  { 550, 48, O_BLOCK},
  { 600, (uint8_t)(GROUND_Y - 34), O_PIPE},
  { 670, 70, O_COIN},
  { 800, 48, O_BLOCK},
  { 815, 48, O_QBLOCK},
  { 830, 48, O_BLOCK},
  { 900, (uint8_t)(GROUND_Y - 34), O_PIPE},
  { 960, 48, O_COIN},

  // end stairs (4 steps) + flag
  {1040, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  {1055, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  {1055, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  {1070, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  {1070, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  {1070, (uint8_t)(GROUND_Y - 45), O_BLOCK},
  {1085, (uint8_t)(GROUND_Y - 15), O_BLOCK},
  {1085, (uint8_t)(GROUND_Y - 30), O_BLOCK},
  {1085, (uint8_t)(GROUND_Y - 45), O_BLOCK},
  {1085, (uint8_t)(GROUND_Y - 60), O_BLOCK},
  {FLAG_X, (uint8_t)(GROUND_Y - 72), O_FLAG},
};
static_assert(sizeof(WORLD) / sizeof(WORLD[0]) == WORLD_COUNT,
              "WORLD_COUNT does not match WORLD[]");

const EnemyDef SPAWNS[] PROGMEM = {
  {120, E_GOOMBA},
  {280, E_GOOMBA},
  {450, E_KOOPA},
  {700, E_GOOMBA},
  {860, E_KOOPA},
};
static_assert(sizeof(SPAWNS) / sizeof(SPAWNS[0]) == SPAWN_COUNT,
              "SPAWN_COUNT does not match SPAWNS[]");

BrokenBrick brokenBricks[MAX_BROKEN];
uint8_t brokenCount = 0;
UsedQBlock usedQBlocks[MAX_USED_Q];
uint8_t usedQCount = 0;
EraseRect pendingErase[MAX_ERASE];
uint8_t pendingEraseCount = 0;

static const uint8_t OBJ_W[] PROGMEM = {28, 51, 51, 15, 15, 9, 24, 16};
static const uint8_t OBJ_H[] PROGMEM = {15, 15, 15, 15, 15, 15, 34, 72};

uint8_t objWidth(uint8_t t)  { return pgm_read_byte(&OBJ_W[t]); }
uint8_t objHeight(uint8_t t) { return pgm_read_byte(&OBJ_H[t]); }

bool objSolid(uint8_t t) {
  return t == O_BLOCK || t == O_QBLOCK || t == O_PIPE;
}

static bool slotMarked(const BrokenBrick* list, uint8_t count,
                       int32_t section, uint8_t index) {
  for (uint8_t i = 0; i < count; i++) {
    if (list[i].section == (int16_t)section && list[i].index == index) {
      return true;
    }
  }
  return false;
}

bool isBroken(int32_t section, uint8_t index) {
  return slotMarked(brokenBricks, brokenCount, section, index);
}

bool isUsedQ(int32_t section, uint8_t index) {
  return slotMarked(usedQBlocks, usedQCount, section, index);
}

void queueBlockErase(int32_t section, uint8_t index, uint8_t type) {
  if (pendingEraseCount >= MAX_ERASE) return;
  EraseRect& r = pendingErase[pendingEraseCount++];
  r.x = section * SECTION_W + (int16_t)pgm_read_word(&WORLD[index].x);
  r.y = pgm_read_byte(&WORLD[index].y);
  r.w = objWidth(type);
  r.h = objHeight(type);
}

void markGone(int32_t section, uint8_t index, uint8_t type) {
  if (isBroken(section, index)) return;

  if (brokenCount < MAX_BROKEN) {
    brokenBricks[brokenCount].section = (int16_t)section;
    brokenBricks[brokenCount].index = index;
    brokenCount++;
  } else {
    for (uint8_t i = 1; i < MAX_BROKEN; i++) brokenBricks[i - 1] = brokenBricks[i];
    brokenBricks[MAX_BROKEN - 1].section = (int16_t)section;
    brokenBricks[MAX_BROKEN - 1].index = index;
  }

  queueBlockErase(section, index, type);
}
