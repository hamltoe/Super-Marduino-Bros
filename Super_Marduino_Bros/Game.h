#ifndef SUPER_MARDUINO_GAME_H
#define SUPER_MARDUINO_GAME_H

#include "Config.h"
#include "Input.h"

extern Enemy enemies[];
extern Item items[];

extern uint16_t score;
extern uint16_t timeLeft;
extern uint8_t timeTick;

extern int32_t spawnFrontier;
extern uint8_t animTick;
extern uint8_t enemyFrame;

extern bool bigMario;
extern uint8_t invulnTicks;

inline int16_t playerH() {
  return bigMario ? PLAYER_H_BIG : PLAYER_H_SMALL;
}

inline int32_t playerHQ() {
  return (int32_t)playerH() << 8;
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
void spawnEnemies();
void updateEnemies();
void collideShellHits();
void collideEnemies();
void updateItems();
void collideItems();
void collideCoins();
void tickTimer();

#endif
