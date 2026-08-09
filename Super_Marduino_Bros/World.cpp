#include "World.h"
#include <avr/pgmspace.h>

// One 320 px section of world, painted in table order so later
// entries cover earlier ones. Parallax is gone: hardware scrolling
// slides the whole GRAM at once, so the backdrop is now part of the
// world and repeats with it.
const ObjDef WORLD[] PROGMEM = {
  { 15, 17, O_CLOUD},
  {190, 31, O_CLOUD},
  {  5,  0, O_HILL},
  { 67,  0, O_HILL2},
  { 72, 59, O_BLOCK},
  { 87, 59, O_QBLOCK},
  {102, 59, O_BLOCK},
  {125, 48, O_COIN},
  {145, 70, O_COIN},
  {166, (uint8_t)(GROUND_Y - 34), O_PIPE},
  {200, 48, O_COIN},
  {230, 59, O_QBLOCK},
  {250, 30, O_COIN},
  {267, 48, O_BLOCK},
  {282, 48, O_BLOCK},
};
static_assert(sizeof(WORLD) / sizeof(WORLD[0]) == WORLD_COUNT,
              "WORLD_COUNT does not match WORLD[]");

// Loot for each WORLD entry. Only O_QBLOCK slots are read on head-hit;
// both question blocks hold a mushroom.
const uint8_t WORLD_LOOT[] PROGMEM = {
  Q_NONE, Q_NONE, Q_NONE, Q_NONE,
  Q_NONE, Q_MUSHROOM, Q_NONE, Q_NONE,
  Q_NONE, Q_NONE, Q_NONE, Q_MUSHROOM,
  Q_NONE, Q_NONE, Q_NONE,
};

// The World 1-1 cast, laid out inside the same repeating 320 px
// section as the scenery.
const EnemyDef SPAWNS[] PROGMEM = {
  {120, E_GOOMBA},
  {205, E_KOOPA},
  {250, E_GOOMBA},
  {272, E_GOOMBA},
};
static_assert(sizeof(SPAWNS) / sizeof(SPAWNS[0]) == SPAWN_COUNT,
              "SPAWN_COUNT does not match SPAWNS[]");

BrokenBrick brokenBricks[MAX_BROKEN];
uint8_t brokenCount = 0;
UsedQBlock usedQBlocks[MAX_USED_Q];
uint8_t usedQCount = 0;
EraseRect pendingErase[MAX_ERASE];
uint8_t pendingEraseCount = 0;

static const uint8_t OBJ_W[] PROGMEM = {28, 51, 51, 15, 15, 9, 24};
static const uint8_t OBJ_H[] PROGMEM = {15, 15, 15, 15, 15, 15, 34};

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

// Record a gone WORLD slot and queue a one-shot GRAM rewrite of its rect.
// composeColumn skips it afterward so dirty paints do not put it back.
void markGone(int32_t section, uint8_t index, uint8_t type) {
  if (isBroken(section, index)) return;

  if (brokenCount < MAX_BROKEN) {
    brokenBricks[brokenCount].section = (int16_t)section;
    brokenBricks[brokenCount].index = index;
    brokenCount++;
  } else {
    // Ring: drop the oldest. Its GRAM hole stays until that column is
    // dirtied again, at which point the object can reappear - rare.
    for (uint8_t i = 1; i < MAX_BROKEN; i++) brokenBricks[i - 1] = brokenBricks[i];
    brokenBricks[MAX_BROKEN - 1].section = (int16_t)section;
    brokenBricks[MAX_BROKEN - 1].index = index;
  }

  queueBlockErase(section, index, type);
}
