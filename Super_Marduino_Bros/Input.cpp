#include "Input.h"
#include <Wire.h>

#define NES_I2C_ADDR ((uint8_t)0x52)

bool controllerOk = false;

bool initController() {
  Wire.begin();
  Wire.setClock(I2C_CLOCK);

  Wire.beginTransmission(NES_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    return false;
  }

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xF0);
  Wire.write(0x55);
  Wire.endTransmission();
  delay(10);

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xFB);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(10);

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0xFE);
  Wire.write(0x03);
  Wire.endTransmission();
  delay(10);

  return true;
}

Buttons readController() {
  Buttons btn = {};
  if (!controllerOk) {
    return btn;
  }

  Wire.beginTransmission(NES_I2C_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    return btn;
  }

  uint8_t data[8];
  uint8_t got = Wire.requestFrom(NES_I2C_ADDR, (uint8_t)8);
  if (got < 6) {
    while (Wire.available()) Wire.read();
    return btn;
  }
  for (uint8_t i = 0; i < got && i < 8; i++) {
    data[i] = Wire.read();
  }
  while (Wire.available()) Wire.read();

  uint8_t b0 = (got >= 8) ? data[6] : data[4];
  uint8_t b1 = (got >= 8) ? data[7] : data[5];

  btn.right  = !(b0 & 0x80);
  btn.down   = !(b0 & 0x40);
  btn.select = !(b0 & 0x10);
  btn.start  = !(b0 & 0x04);
  btn.b      = !(b1 & 0x40);
  btn.a      = !(b1 & 0x10);
  btn.left   = !(b1 & 0x02);
  btn.up     = !(b1 & 0x01);

  return btn;
}
