// ESC LightStick — Gateway 型別定義
// 封包格式 (Packet) 必須與 master / gateway / lightstick 三支韌體完全相同
#pragma once
#include <stdint.h>

typedef struct {
  uint8_t  ver;
  uint8_t  msgId;
  uint8_t  targetGroup;
  uint8_t  mode;
  uint8_t  brightness;
  uint8_t  reserved;
  uint16_t bpm;
  uint16_t fadeMs;
  uint16_t seq;
  uint32_t color;
  float    speed;
  float    spread;
  float    duty;
  uint32_t pal[4];
  uint32_t timestamp;
  uint32_t applyAt;
} Packet;
