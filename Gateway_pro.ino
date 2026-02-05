/*
   [Gateway] 中繼站 - Final v2
   - 修正心跳被阻擋問題
   - 支援隨機避讓
*/
#include <ESP8266WiFi.h>
#include <espnow.h>

// 資料結構 (必須一致)
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
  uint32_t pal[4];
  uint32_t timestamp; 
} struct_message;

struct_message myData;
struct_message incomingData;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define STATUS_LED D4
uint8_t lastMsgId = 255; 
unsigned long lastRelayTime = 0; 

void OnDataRecv(uint8_t * mac, uint8_t *incomingDataPtr, uint8_t len);

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH); 
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != 0) { ESP.restart(); }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, 1, NULL, 0);
  esp_now_register_recv_cb(OnDataRecv);
  wifi_set_channel(1); 
  Serial.println(">>> Gateway Ready (v2 Fix) <<<");
}

void loop() {}

void OnDataRecv(uint8_t * mac, uint8_t *incomingDataPtr, uint8_t len) {
  if (len != sizeof(incomingData)) return;
  memcpy(&incomingData, incomingDataPtr, sizeof(incomingData));
  
  bool shouldRelay = false;
  bool isNewCommand = false;

  if (incomingData.msgId != lastMsgId) {
    shouldRelay = true;
    isNewCommand = true;
    lastMsgId = incomingData.msgId;
  } else if (millis() - lastRelayTime > 200) {
    shouldRelay = true; // 放行心跳
    isNewCommand = false;
  }

  if (shouldRelay) {
    digitalWrite(STATUS_LED, LOW);
    delayMicroseconds(random(1000, 4000)); // 避讓
    
    esp_now_send(broadcastAddress, (uint8_t *) &incomingData, sizeof(incomingData));
    
    if (isNewCommand) { // 新指令補發一槍
      delay(5);
      esp_now_send(broadcastAddress, (uint8_t *) &incomingData, sizeof(incomingData));
    }
    lastRelayTime = millis();
    digitalWrite(STATUS_LED, HIGH);
  }
}