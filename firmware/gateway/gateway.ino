// =====================================================================
//  ESC LightStick — 中繼節點 Gateway 
//  封包格式必須與 Master / 手燈 一致
// =====================================================================
#include <ESP8266WiFi.h>
#include <espnow.h>

#define PROTO_VER    3
#define WIFI_CHANNEL 1

// ===================== 封包（Master / Gateway / 手燈 必須完全相同） =====================
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

Packet rx;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

#define STATUS_LED D4
uint8_t lastMsgId = 255;
unsigned long lastRelayTime = 0;

void OnDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(Packet)) return;
  memcpy(&rx, data, sizeof(rx));
  if (rx.ver != PROTO_VER) return;

  bool shouldRelay = false;
  bool isNewCommand = false;

  if (rx.msgId != lastMsgId) {
    shouldRelay = true;
    isNewCommand = true;
    lastMsgId = rx.msgId;
  } else if (millis() - lastRelayTime > 200) {
    shouldRelay = true;
  }

  if (shouldRelay) {
    digitalWrite(STATUS_LED, LOW);
    delayMicroseconds(random(1000, 4000));   // 隨機避讓，避免多台 Gateway 同時轉發
    esp_now_send(broadcastAddress, (uint8_t *)&rx, sizeof(rx));
    if (isNewCommand) {
      delay(5);
      esp_now_send(broadcastAddress, (uint8_t *)&rx, sizeof(rx));
    }
    lastRelayTime = millis();
    digitalWrite(STATUS_LED, HIGH);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, HIGH);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_now_init() != 0) { ESP.restart(); }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_add_peer(broadcastAddress, ESP_NOW_ROLE_SLAVE, WIFI_CHANNEL, NULL, 0);
  esp_now_register_recv_cb(OnDataRecv);
  wifi_set_channel(WIFI_CHANNEL);
  Serial.println(">>> Gateway Ready (v3) <<<");
}

void loop() {}
