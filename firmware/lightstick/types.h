// ESC LightStick — 手燈型別定義
// 封包格式 (Packet) 必須與 master / gateway / lightstick 三支韌體完全相同
#pragma once
#include <stdint.h>

struct Config {
  uint32_t magic;
  char     ssid[33];   // 最多 32 bytes + '\0'
  char     pass[64];   // 最多 63 bytes + '\0'
  uint8_t  group;      // 1–10
};

struct State {
  uint8_t  mode;
  uint8_t  brightness;
  uint16_t bpm;
  uint32_t color;
  float    speed;
  float    spread;
  float    duty;
  uint32_t pal[4];
};

typedef struct {
  uint8_t  ver;          // 協定版本 = 3
  uint8_t  msgId;
  uint8_t  targetGroup;  // 0 = 全部, 1–10
  uint8_t  mode;
  uint8_t  brightness;
  uint8_t  reserved;
  uint16_t bpm;
  uint16_t fadeMs;
  uint16_t seq;          // 狀態版本
  uint32_t color;
  float    speed;
  float    spread;
  float    duty;
  uint32_t pal[4];
  uint32_t timestamp;    // Master millis()（校時用）
  uint32_t applyAt;      // 要在 Master 的哪個時間點套用
} Packet;
