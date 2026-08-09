#ifndef SUPER_MARDUINO_WORLD_H
#define SUPER_MARDUINO_WORLD_H

#include "Config.h"

extern const ObjDef WORLD[];
extern const EnemyDef SPAWNS[];

// Keep in sync with the PROGMEM tables in World.cpp (AVR const = RAM).
#define WORLD_COUNT 41
#define SPAWN_COUNT 5

extern BrokenBrick brokenBricks[];
extern uint8_t brokenCount;
extern UsedQBlock usedQBlocks[];
extern uint8_t usedQCount;
extern EraseRect pendingErase[];
extern uint8_t pendingEraseCount;

uint8_t objWidth(uint8_t t);
uint8_t objHeight(uint8_t t);
bool objSolid(uint8_t t);
bool isBroken(int32_t section, uint8_t index);
bool isUsedQ(int32_t section, uint8_t index);
void queueBlockErase(int32_t section, uint8_t index, uint8_t type);
void markGone(int32_t section, uint8_t index, uint8_t type);

#endif
