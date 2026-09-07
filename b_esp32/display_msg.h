#pragma once

#include <stdint.h>

typedef struct {
  uint8_t kind;  // 0=sensor TCP, 1=joystick UART
  float temp, humi;
  int dist;
  uint16_t joyX, joyY;
  uint8_t joyDir, joySw;
} DisplayMsg;
