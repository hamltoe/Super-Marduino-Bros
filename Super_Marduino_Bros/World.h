#ifndef SUPER_MARDUINO_WORLD_H
#define SUPER_MARDUINO_WORLD_H

#include "Config.h"

// Cached copy of the active course (one LevelDef in SRAM, tables in flash).
extern const ObjDef* worldObjs;
extern const EnemyDef* worldSpawns;
extern uint8_t worldCount;
extern uint8_t spawnCount;
extern int16_t levelW;
extern int16_t flagX;
extern uint8_t flagTop;
extern uint8_t levelSpawnY;
extern uint8_t levelTheme;
extern uint8_t levelFlags;
extern uint8_t levelIdx;

extern uint16_t palSky;
extern uint16_t palBrick;
extern uint16_t palBrickDk;
extern uint16_t palDirt;
extern uint16_t palGrass;

extern Beam beams[];
extern uint8_t beamCount;

extern BrokenBrick brokenBricks[];
extern uint8_t brokenCount;
extern UsedQBlock usedQBlocks[];
extern uint8_t usedQCount;
extern EraseRect pendingErase[];
extern uint8_t pendingEraseCount;

void loadLevel(uint8_t idx);

uint8_t objWidth(uint8_t t);
uint8_t objHeight(uint8_t t);
bool objSolid(uint8_t t);
bool isBroken(int32_t section, uint8_t index);
bool isUsedQ(int32_t section, uint8_t index);
void queueBlockErase(int32_t section, uint8_t index, uint8_t type);
void markGone(int32_t section, uint8_t index, uint8_t type);

extern const LevelDef LEVEL_OVER PROGMEM;
extern const LevelDef LEVEL_UNDER PROGMEM;
extern const LevelDef LEVEL_MUSH PROGMEM;
extern const LevelDef LEVEL_WATER PROGMEM;
extern const LevelDef LEVEL_CASTLE PROGMEM;

#endif
