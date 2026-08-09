#ifndef SUPER_MARDUINO_UI_H
#define SUPER_MARDUINO_UI_H

#include "Config.h"

void enterMode(uint8_t mode);
void updateTitle(bool eStart, bool eA, bool startDown, bool aDown);
void updateSelect(bool eStart, bool eA, bool eLeft, bool eRight,
                  bool eUp, bool eDown, bool startDown, bool aDown);
void updateLives(bool eStart, bool startDown, bool aDown);
void updateDead(bool eStart, bool startDown, bool aDown);

#endif
