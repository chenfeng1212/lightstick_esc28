/*
   [Master] 中控發射端 - Final v4
   - 支援 4色漸變
   - 支援 Time Sync (時間同步)
   - 支援 Burst Fire (連發抗掉包)
   - 支援 STOP 指令
*/
#include <ESP8266WiFi.h>
#include <espnow.h>

// ===================== 資料結構 (全系統必須一致) =====================
typedef struct struct_message {
  uint8_t msgId;       
  uint8_t targetGroup; 
  uint8_t mode;        
  uint8_t brightness;
  uint16_t bpm;
  uint32_t color;
  float speed;
  float spread;
  float duty;
  uint32_t pal[4];    // 漸變色盤
  uint32_t timestamp; // Master 的時間戳記
} struct_message;

struct_message myData;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t currentMsgId = 0;
unsigned long lastHeartbeatTime = 0;
bool hasActiveCommand = false;

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW Init Failed");
    return;
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_CONTROLLER);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  wifi_set_channel(1); 
  
  Serial.println(">>> Master Ready (Final v4) <<<");
}

void loop() {
  // 1. 監聽電腦指令
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');
    input.trim();
    if (input.length() > 0) {
      if (input == "STOP") {
        hasActiveCommand = false;
        Serial.println("BROADCAST STOPPED");
      } else {
        parseAndSend(input);
        hasActiveCommand = true;
      }
    }
  }

  // 2. 自動重發心跳 (Heartbeat)
  if (hasActiveCommand && (millis() - lastHeartbeatTime > 500)) {
    myData.timestamp = millis(); // 更新為最新時間
    esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
    lastHeartbeatTime = millis();
  }
}

void parseAndSend(String input) {
  // 預設值
  int group = 0; int mode = 1; int bri = 200; int bpm = 120;
  String colorHex = "00CCFF"; float spd = 1.2; float spr = 0.6; float dty = 0.5;
  String p[4] = {"FF0000", "00FF00", "0000FF", "FFFFFF"};

  // 解析字串
  int idx[12]; int found = 0; idx[0] = input.indexOf(',');
  while(idx[found] != -1 && found < 11) { found++; idx[found] = input.indexOf(',', idx[found-1] + 1); }
  
  if (found >= 0 && idx[0] != -1) group = input.substring(0, idx[0]).toInt();
  if (found >= 1 && idx[1] != -1) mode = input.substring(idx[0] + 1, idx[1]).toInt();
  if (found >= 2 && idx[2] != -1) bri = input.substring(idx[1] + 1, idx[2]).toInt();
  if (found >= 3 && idx[3] != -1) bpm = input.substring(idx[2] + 1, idx[3]).toInt();
  if (found >= 4 && idx[4] != -1) colorHex = input.substring(idx[3] + 1, idx[4]);
  if (found >= 5 && idx[5] != -1) spd = input.substring(idx[4] + 1, idx[5]).toFloat();
  if (found >= 6 && idx[6] != -1) spr = input.substring(idx[5] + 1, idx[6]).toFloat();
  if (found >= 7 && idx[7] != -1) dty = input.substring(idx[6] + 1, idx[7]).toFloat();
  if (found >= 8 && idx[8] != -1) p[0] = input.substring(idx[7] + 1, idx[8]); else if (found >= 7) p[0] = input.substring(idx[7] + 1);
  if (found >= 9 && idx[9] != -1) p[1] = input.substring(idx[8] + 1, idx[9]); else if (found >= 8) p[1] = input.substring(idx[8] + 1);
  if (found >= 10 && idx[10] != -1) p[2] = input.substring(idx[9] + 1, idx[10]); else if (found >= 9) p[2] = input.substring(idx[9] + 1);
  if (found >= 10) p[3] = input.substring(idx[10] + 1);

  currentMsgId++; 
  myData.msgId = currentMsgId;
  myData.targetGroup = group;
  myData.mode = mode;
  myData.brightness = bri;
  myData.bpm = bpm;
  myData.color = strtoul(colorHex.c_str(), NULL, 16);
  myData.speed = spd;
  myData.spread = spr;
  myData.duty = dty;
  for(int i=0; i<4; i++) myData.pal[i] = strtoul(p[i].c_str(), NULL, 16);
  
  // 加入時間戳記
  myData.timestamp = millis();

  // === 機關槍連發 (Burst Fire) ===
  for (int i = 0; i < 5; i++) {
    esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
    delay(10);
  }
  
  lastHeartbeatTime = millis();
  Serial.println("OK"); 
}