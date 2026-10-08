// ESC LightStick — Master 型別定義
// 封包格式 (Packet) 必須與 master / gateway / lightstick 三支韌體完全相同
#pragma once
#include <stdint.h>

typedef struct {
  uint8_t  ver;          // 協定版本 = 3
  uint8_t  msgId;        // 每次傳輸遞增（Gateway 去重用）
  uint8_t  targetGroup;  // 0 = 全部, 1–10
  uint8_t  mode;
  uint8_t  brightness;
  uint8_t  reserved;
  uint16_t bpm;
  uint16_t fadeMs;       // 淡入時間
  uint16_t seq;          // 狀態版本；相同 seq 代表同一個狀態（心跳重送）
  uint32_t color;
  float    speed;
  float    spread;
  float    duty;
  uint32_t pal[4];
  uint32_t timestamp;    // 送出當下的 Master millis()
  uint32_t applyAt;      // 要在 Master millis() 的哪一刻套用
} Packet;

struct Slot {
  bool     used;
  uint8_t  mode, brightness;
  uint16_t bpm, fadeMs, seq;
  uint32_t color;
  float    speed, spread, duty;
  uint32_t pal[4];
  uint32_t applyAt;
};
