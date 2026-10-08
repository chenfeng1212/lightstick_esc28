// =====================================================================
//  ESC LightStick — 主發射節點 Master (protocol v3)
//  - 記住 10 個群組各自的狀態，心跳輪流補送（漏收 / 晚開機的手燈也能補上）
//  - 電腦先送一批 S 指令暫存，再用 GO 一次廣播；封包帶 applyAt，
//    所有手燈在同一個 Master 時間點切換
//  - 支援淡入時間 fadeMs
//
//  序列埠協定（每行一個指令，\n 結尾）：
//    S,gid,mode,bri,bpm,color,spd,spr,dty,p1,p2,p3,p4   暫存群組狀態（gid 0 = 全部）
//    GO,fadeMs                                          把暫存的群組一次送出
//    STOP                                               停止廣播與心跳
//    gid,mode,bri,bpm,color,spd,spr,dty,p1,p2,p3,p4     v2 舊格式（相容：等同 S + GO,0）
// =====================================================================
#include <ESP8266WiFi.h>
#include <espnow.h>
#include "types.h"  

#define PROTO_VER       3
#define NUM_GROUPS      10
#define WIFI_CHANNEL    1
#define BURST_COUNT     3     // 每個群組新指令連發次數
#define BURST_GAP_MS    3
#define HEARTBEAT_MS    50    // 每 50ms 補送一個群組 → 每群組約 0.5 秒更新一次

// ===================== 封包（Master / Gateway / 手燈 必須完全相同） =====================


Slot slots[NUM_GROUPS + 1];      // index 1–10
Slot staged[NUM_GROUPS + 1];
bool isStaged[NUM_GROUPS + 1];

uint8_t  broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t  msgId = 0;
uint16_t globalSeq;
bool     active = false;
uint8_t  hbIndex = 1;
unsigned long lastHeartbeat = 0;

char lineBuf[200];
uint8_t lineLen = 0;

// ---------------------------------------------------------------------
void fillPacket(Packet &p, uint8_t target, const Slot &s) {
  p.ver = PROTO_VER;
  p.msgId = ++msgId;
  p.targetGroup = target;
  p.mode = s.mode;
  p.brightness = s.brightness;
  p.reserved = 0;
  p.bpm = s.bpm;
  p.fadeMs = s.fadeMs;
  p.seq = s.seq;
  p.color = s.color;
  p.speed = s.speed;
  p.spread = s.spread;
  p.duty = s.duty;
  for (int i = 0; i < 4; i++) p.pal[i] = s.pal[i];
  p.applyAt = s.applyAt;
  p.timestamp = millis();
}

void sendBurst(uint8_t target, const Slot &s) {
  Packet p;
  fillPacket(p, target, s);              // 同一次連發共用 msgId
  for (int i = 0; i < BURST_COUNT; i++) {
    p.timestamp = millis();
    esp_now_send(broadcastAddress, (uint8_t *)&p, sizeof(p));
    delay(BURST_GAP_MS);
  }
}

bool sameState(const Slot &a, const Slot &b) {
  if (a.mode != b.mode || a.brightness != b.brightness || a.bpm != b.bpm || a.color != b.color) return false;
  if (a.speed != b.speed || a.spread != b.spread || a.duty != b.duty) return false;
  for (int i = 0; i < 4; i++) if (a.pal[i] != b.pal[i]) return false;
  return true;
}

// ---------------------------------------------------------------------
// 解析 "mode,bri,bpm,color,spd,spr,dty,p1,p2,p3,p4"（tok 已指向 mode）
bool parseState(char *rest, Slot &s) {
  char *f[11]; int n = 0;
  char *save;
  for (char *t = strtok_r(rest, ",", &save); t && n < 11; t = strtok_r(NULL, ",", &save)) f[n++] = t;
  if (n < 7) return false;
  s.mode       = atoi(f[0]);
  s.brightness = constrain(atoi(f[1]), 0, 255);
  s.bpm        = constrain(atoi(f[2]), 1, 400);
  s.color      = strtoul(f[3], NULL, 16);
  s.speed      = atof(f[4]);
  s.spread     = atof(f[5]);
  s.duty       = atof(f[6]);
  for (int i = 0; i < 4; i++) s.pal[i] = (n > 7 + i) ? strtoul(f[7 + i], NULL, 16) : 0;
  return true;
}

void stage(int gid, const Slot &s) {
  if (gid == 0) { for (int g = 1; g <= NUM_GROUPS; g++) { staged[g] = s; isStaged[g] = true; } }
  else if (gid >= 1 && gid <= NUM_GROUPS) { staged[gid] = s; isStaged[gid] = true; }
}

void commit(uint16_t fadeMs) {
  int n = 0;
  for (int g = 1; g <= NUM_GROUPS; g++) if (isStaged[g]) n++;
  if (n == 0) { Serial.println("OK GO 0"); return; }

  // 10 個群組都暫存且狀態相同 → 只送一個「全部」封包
  bool allSame = (n == NUM_GROUPS);
  for (int g = 2; allSame && g <= NUM_GROUPS; g++) if (!sameState(staged[g], staged[1])) allSame = false;

  int packets = allSame ? 1 : n;
  uint32_t applyAt = millis() + 30 + packets * (BURST_COUNT * BURST_GAP_MS + 1);
  uint16_t seq = ++globalSeq;

  for (int g = 1; g <= NUM_GROUPS; g++) {
    if (!isStaged[g]) continue;
    slots[g] = staged[g];
    slots[g].used = true;
    slots[g].fadeMs = fadeMs;
    slots[g].seq = seq;
    slots[g].applyAt = applyAt;
    isStaged[g] = false;
  }

  if (allSame) sendBurst(0, slots[1]);
  else for (int g = 1; g <= NUM_GROUPS; g++) if (slots[g].seq == seq) sendBurst(g, slots[g]);

  active = true;
  lastHeartbeat = millis();
  Serial.printf("OK GO %d\n", n);
}

void handleLine(char *line) {
  if (strcmp(line, "STOP") == 0) {
    active = false;
    for (int g = 1; g <= NUM_GROUPS; g++) isStaged[g] = false;
    Serial.println("BROADCAST STOPPED");
    return;
  }
  if (strncmp(line, "GO", 2) == 0) {
    uint16_t fade = (line[2] == ',') ? constrain(atoi(line + 3), 0, 10000) : 0;
    commit(fade);
    return;
  }
  if (line[0] == 'S' && line[1] == ',') {
    char *p = line + 2;
    int gid = atoi(p);
    char *rest = strchr(p, ',');
    Slot s = {};
    if (!rest || !parseState(rest + 1, s)) { Serial.println("ERR S"); return; }
    stage(gid, s);
    return;
  }
  if (line[0] >= '0' && line[0] <= '9') {  
    int gid = atoi(line);
    char *rest = strchr(line, ',');
    Slot s = {};
    if (!rest || !parseState(rest + 1, s)) { Serial.println("ERR"); return; }
    stage(gid, s);
    commit(0);
    return;
  }
  Serial.println("ERR UNKNOWN");
}

// ---------------------------------------------------------------------
void setup() {
  Serial.setRxBufferSize(2048);   // 一次 GO 10 個群組約 750 bytes，預設 256 bytes 會溢位
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, WIFI_CHANNEL, NULL, 0);
  wifi_set_channel(WIFI_CHANNEL);

  // 隨機起始序號：Master 重開機後，手燈不會誤以為是舊狀態
  globalSeq = (uint16_t)(RANDOM_REG32 & 0xFFFF);
  memset(slots, 0, sizeof(slots));
  memset(isStaged, 0, sizeof(isStaged));

  Serial.println(">>> Master Ready (v3) <<<");
}

void loop() {
  // 非阻塞讀取一行
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      lineBuf[lineLen] = '\0';
      if (lineLen > 0) handleLine(lineBuf);
      lineLen = 0;
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }

  // 心跳：輪流補送已設定過的群組（同一個 seq，手燈不會重新淡入，只會校時）
  if (active && millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    for (int tries = 0; tries < NUM_GROUPS; tries++) {
      uint8_t g = hbIndex;
      hbIndex = hbIndex % NUM_GROUPS + 1;
      if (slots[g].used) {
        Packet p;
        fillPacket(p, g, slots[g]);
        esp_now_send(broadcastAddress, (uint8_t *)&p, sizeof(p));
        break;
      }
    }
  }
}
