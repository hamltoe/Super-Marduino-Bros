#ifndef SUPER_MARDUINO_GAME_H
#define SUPER_MARDUINO_GAME_H

#include "Config.h"
#include "Input.h"

extern Enemy enemies[];

extern uint16_t score;

extern int32_t spawnFrontier;
extern uint8_t animTick;
extern uint8_t enemyFrame;

inline int16_t playerH() {
  return PLAYER_H_SMALL;
}

inline int32_t playerHQ() {
  return (int32_t)PLAYER_H_SMALL << 8;
}

extern int32_t playerXq;
extern int32_t playerYq;
extern int16_t velXq;
extern int16_t velYq;
extern bool onGround;
extern bool facingRight;
extern bool jumpWasHeld;
extern uint8_t animFrame;
extern int32_t cameraX;

extern uint8_t gameMode;
extern uint8_t playState;
extern uint8_t lives;
extern bool playAsLuigi;
extern uint8_t selectIdx;
extern uint32_t modeMs;
extern uint32_t deathMs;
extern bool deathNeedsRender;
extern bool uiDirty;
extern bool menuArmed;

extern bool prevStart;
extern bool prevA;
extern bool prevLeft;
extern bool prevRight;
extern bool prevUp;
extern bool prevDown;

extern uint32_t lastStep;

#if DEBUG_SERIAL
extern uint32_t lastReport;
extern uint16_t frameCount;
extern uint32_t pixelCount;
#endif

int16_t enemyWidth(const Enemy& e);
int16_t enemyHeight(const Enemy& e);
int16_t enemyMaxHeight(const Enemy& e);

void resetLevel();
void updatePlayer(const Buttons& btn);
void updateDeathFall();
void updateBeams();
void spawnEnemies();
void updateEnemies();
void collideEnemies();
void collideCoins();
void collideFlag();

#endif
