#include "World.h"
#include <avr/pgmspace.h>

static const LevelDef* const LEVEL_TABLE[] PROGMEM = {
  &LEVEL_OVER, &LEVEL_UNDER, &LEVEL_MUSH, &LEVEL_WATER, &LEVEL_CASTLE
};

const ObjDef* worldObjs;
const EnemyDef* worldSpawns;
uint8_t worldCount;
uint8_t spawnCount;
int16_t levelW;
int16_t flagX;
uint8_t flagTop;
uint8_t levelSpawnY;
uint8_t levelTheme;
uint8_t levelFlags;
uint8_t levelIdx = 0;

uint16_t palSky = SKY_BLUE;
uint16_t palBrick = ORANGE;
uint16_t palBrickDk = DARK_DIRT;
uint16_t palDirt = DIRT;
uint16_t palGrass = GRASS;

Beam beams[MAX_BEAMS];
uint8_t beamCount = 0;

BrokenBrick brokenBricks[MAX_BROKEN];
uint8_t brokenCount = 0;
UsedQBlock usedQBlocks[MAX_USED_Q];
uint8_t usedQCount = 0;
EraseRect pendingErase[MAX_ERASE];
uint8_t pendingEraseCount = 0;

static const uint8_t OBJ_W[] PROGMEM = {28, 51, 51, 15, 15, 9, 24, 16, 40};
static const uint8_t OBJ_H[] PROGMEM = {15, 15, 15, 15, 15, 15, 34, 72, 8};

uint8_t objWidth(uint8_t t)  { return pgm_read_byte(&OBJ_W[t]); }
uint8_t objHeight(uint8_t t) { return pgm_read_byte(&OBJ_H[t]); }

bool objSolid(uint8_t t) {
  return t == O_BLOCK || t == O_QBLOCK || t == O_PLAT;
}

static void setPalette(uint8_t theme) {
  switch (theme) {
    case TH_UNDER:
      palSky = BLACK;
      palBrick = BRICK_BLUE;
      palBrickDk = BRICK_BLUE_DK;
      palDirt = UNDER_FLOOR;
      palGrass = BRICK_BLUE_DK;
      break;
    case TH_WATER:
      palSky = WATER_SKY;
      palBrick = ORANGE;
      palBrickDk = DARK_DIRT;
      palDirt = WATER_SAND;
      palGrass = WATER_SAND;
      break;
    case TH_CASTLE:
      palSky = BLACK;
      palBrick = CASTLE_BRICK;
      palBrickDk = CASTLE_BRICK_DK;
      palDirt = BLACK;
      palGrass = CASTLE_BRICK_DK;
      break;
    default:
      palSky = SKY_BLUE;
      palBrick = ORANGE;
      palBrickDk = DARK_DIRT;
      palDirt = DIRT;
      palGrass = GRASS;
      break;
  }
}

void loadLevel(uint8_t idx) {
  if (idx >= LEVEL_COUNT) idx = 0;
  levelIdx = idx;

  const LevelDef* src = (const LevelDef*)pgm_read_word(&LEVEL_TABLE[idx]);
  worldObjs = (const ObjDef*)pgm_read_word(&src->objs);
  worldSpawns = (const EnemyDef*)pgm_read_word(&src->spawns);
  const BeamDef* beamSrc = (const BeamDef*)pgm_read_word(&src->beams);
  levelW = (int16_t)pgm_read_word(&src->width);
  worldCount = pgm_read_byte(&src->objCount);
  spawnCount = pgm_read_byte(&src->spawnCount);
  beamCount = pgm_read_byte(&src->beamCount);
  levelSpawnY = pgm_read_byte(&src->spawnY);
  levelTheme = pgm_read_byte(&src->theme);
  levelFlags = pgm_read_byte(&src->flags);
  if (beamCount > MAX_BEAMS) beamCount = MAX_BEAMS;

  setPalette(levelTheme);

  flagX = (int16_t)(levelW - 16);
  flagTop = (uint8_t)(GROUND_Y - 72);
  for (uint8_t i = 0; i < worldCount; i++) {
    if (pgm_read_byte(&worldObjs[i].type) == O_FLAG) {
      flagX = (int16_t)pgm_read_word(&worldObjs[i].x);
      flagTop = pgm_read_byte(&worldObjs[i].y);
      break;
    }
  }

  for (uint8_t i = 0; i < beamCount; i++) {
    int16_t x0 = (int16_t)pgm_read_word(&beamSrc[i].x0);
    int16_t x1 = (int16_t)pgm_read_word(&beamSrc[i].x1);
    beams[i].xMin = x0;
    beams[i].xMax = x1;
    beams[i].x = x0;
    beams[i].prevX = x0;
    beams[i].y = pgm_read_byte(&beamSrc[i].y);
    beams[i].w = pgm_read_byte(&beamSrc[i].w);
    beams[i].vx = 1;
  }
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
  r.x = (int16_t)pgm_read_word(&worldObjs[index].x);
  r.y = pgm_read_byte(&worldObjs[index].y);
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
