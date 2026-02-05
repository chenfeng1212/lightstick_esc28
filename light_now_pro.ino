#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <FastLED.h>
#include <espnow.h> 

#define LED_PIN     D5      
#define NUM_LEDS    4
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB 

const char* AP_SSID = "LightStick_139"; 
const int   AP_CHANNEL = 1; 
const int   MY_GROUP_ID = 9; 

// 狀態結構
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

State web_state = {0, 200, 120, 0x00CCFF, 1.2f, 0.6f, 0.5f, {0xFF0044, 0xFFD400, 0x00E5FF, 0xFFFFFF}};
State server_state = {0, 0, 0, 0, 0, 0, 0, {0,0,0,0}};

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

struct_message incomingData;

CRGB leds[NUM_LEDS];
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 2);
DNSServer dnsServer;
ESP8266WebServer server(80);

unsigned long last_server_time = 0;
bool is_server_online = false;
long timeOffset = 0; // 時間偏差值

uint32_t parseHex(String s) { s.replace("#", ""); return strtoul(s.c_str(), NULL, 16); }
CRGB lerpColor(uint32_t c1, uint32_t c2, float f) { CRGB a(c1), b(c2); return blend(a, b, f * 255); }

// ESP-NOW 接收
void OnDataRecv(uint8_t * mac, uint8_t *incomingDataPtr, uint8_t len) {
  memcpy(&incomingData, incomingDataPtr, sizeof(incomingData));
  last_server_time = millis();
  
  if (incomingData.targetGroup == 0 || incomingData.targetGroup == MY_GROUP_ID) {
    server_state.mode = incomingData.mode;
    server_state.brightness = incomingData.brightness;
    server_state.bpm = incomingData.bpm;
    server_state.color = incomingData.color;
    server_state.speed = incomingData.speed;
    server_state.spread = incomingData.spread;
    server_state.duty = incomingData.duty;
    for(int i=0; i<4; i++) server_state.pal[i] = incomingData.pal[i];
    
    // 計算與 Master 的時間差
    timeOffset = incomingData.timestamp - millis();
  }
}

// 渲染邏輯
void render(State *st, bool useSyncTime) {
  FastLED.setBrightness(st->brightness);
  
  // 使用同步時間 或 本地時間
  uint32_t now = useSyncTime ? (millis() + timeOffset) : millis();

  uint32_t color = st->color;
  uint16_t bpm = st->bpm;
  float speed = st->speed;
  float spread = st->spread;
  float duty = st->duty;

  switch(st->mode) {
    case 0: fill_solid(leds, NUM_LEDS, CRGB::Black); break;
    case 1: fill_solid(leds, NUM_LEDS, CRGB(color)); break;
    case 2: { // Breathe
        float ph = 2.0f * 3.1415926f * (bpm / 60000.0f) * now;
        float w = (cosf(ph) * 0.5f + 0.5f);
        CRGB c(color); c.nscale8(w * 255); 
        fill_solid(leds, NUM_LEDS, c);
      } break;
    case 3: { // Rainbow
        float base = now * 0.001f * max(0.01f, speed);
        for (int i = 0; i < NUM_LEDS; i++) {
          float hue = fmodf(base + (i * spread) / (float)NUM_LEDS, 1.0f);
          leds[i] = CHSV(hue * 255, 255, 255);
        }
      } break;
    case 4: { // Window
        float pos = fmodf(now * 0.001f * max(0.01f, speed), 1.0f);
        float half = constrain(spread, 0.05f, 1.0f) * 0.5f;
        CRGB baseC(color);
        for (int i = 0; i < NUM_LEDS; i++) {
          float x = (float)i / (NUM_LEDS > 1 ? NUM_LEDS - 1 : 1);
          float d = fabsf(x - pos);
          float w = d < half ? 1.0f : 0.1f;
          leds[i] = baseC; leds[i].nscale8(w * 255);
        }
      } break;
    case 5: { // Beat
        float beat = fmodf((now / 60000.0f) * bpm, 1.0f);
        bool on = beat <= constrain(duty, 0.05f, 0.95f);
        CRGB c(color); if (!on) c.nscale8(13);
        fill_solid(leds, NUM_LEDS, c);
      } break;
    case 7: { // Palette
         float t = now * 0.001f * max(0.01f, speed);
         float pos = fmodf(t, 1.0f) * 4; 
         int i0 = (int)floorf(pos) % 4;
         int i1 = (i0 + 1) % 4;
         float f = pos - floorf(pos);
         CRGB c = lerpColor(st->pal[i0], st->pal[i1], f);
         fill_solid(leds, NUM_LEDS, c);
      } break;
    default: fill_solid(leds, NUM_LEDS, CRGB(color)); break;
  }
}

// HTML
const char* html_page = R"rawliteral(
<!doctype html>
<html lang="zh-Hant">
<meta charset="utf-8"/><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>手燈控制</title>
<style>
  body{margin:0;background:#0f172a;color:#e5e7eb;font-family:sans-serif;text-align:center;user-select:none;}
  .wrap{max-width:600px;margin:20px auto;padding:10px}
  .row{display:flex;gap:8px;align-items:center;margin:10px 0}
  input[type="range"]{flex:1}
  button{border:1px solid #334155;background:#1f2937;color:#fff;border-radius:8px;padding:10px;flex:1;cursor:pointer}
  button:active{transform:translateY(2px);}
  .active{background:#22d3ee;color:#000;font-weight:bold}
  .modes{display:grid;grid-template-columns:1fr 1fr 1fr;gap:5px}
  #tapBtn { flex:0.4; background:#f59e0b; color:#000; font-weight:bold; }
  #bpmVal { min-width:30px; text-align:right; font-family:monospace; }
</style>
<body>
<div class="wrap">
  <h2>ESC手燈控制介面</h2>
  <div class="modes">
    <button class="m" d-m="1">恆亮</button><button class="m" d-m="2">呼吸</button><button class="m" d-m="3">彩虹</button>
    <button class="m" d-m="4">窗口</button><button class="m" d-m="5">節拍</button><button class="m" d-m="7">漸變</button>
    <button class="m" d-m="0" style="background:#ef4444">關閉</button>
  </div>
  
  <div class="row"><label>BPM是調整呼吸和節拍的，速度是調整彩虹、窗口和漸變，Duty是改變節拍亮的比例</label></div>
  
  <div class="row"><label>亮度</label><input id="bri" type="range" max="255" value="200"></div>
  
  <div class="row">
    <label>BPM</label>
    <input id="bpm" type="range" min="10" max="200" value="120">
    <span id="bpmVal">120</span>
    <button id="tapBtn">Tap!</button>
  </div>

  <div class="row"><label>速度</label><input id="spd" type="range" max="300" value="120"></div>
  <div class="row"><label>展開</label><input id="spr" type="range" max="100" value="60"></div>
  <div class="row"><label>Duty</label><input id="dty" type="range" min="5" max="95" value="50"></div>
  <div class="row"><label>主色</label><input id="col" type="color" value="#00ccff" style="width:100%"></div>
  <div class="row" id="pals"><label>色盤</label>
    <input type="color" value="#ff0044"><input type="color" value="#ffd400">
    <input type="color" value="#00e5ff"><input type="color" value="#ffffff">
  </div>
</div>
<script>
const $ = s=>document.querySelector(s); const $$=s=>document.querySelectorAll(s);
let st = {m:2, bri:200, bpm:120, col:'00ccff', spd:1.2, spr:0.6, dty:0.5, p:['ff0044','ffd400','00e5ff','ffffff']};

function send() {
  let url = `/set?m=${st.m}&bri=${st.bri}&bpm=${st.bpm}&c=${st.col}&spd=${st.spd}&spr=${st.spr}&dty=${st.dty}&pal=${st.p.join(',')}`;
  fetch(url).catch(e=>console.log(e));
}

$$('.m').forEach(b=>{
  b.onclick=()=>{ $$('.m').forEach(x=>x.classList.remove('active')); b.classList.add('active'); st.m=b.getAttribute('d-m'); send(); }
});
$('#bri').oninput=e=>{ st.bri=e.target.value; send(); };

$('#bpm').oninput=e=>{ 
  st.bpm=e.target.value; 
  $('#bpmVal').innerText=st.bpm; 
  send(); 
};

$('#spd').oninput=e=>{ st.spd=e.target.value/100; send(); };
$('#spr').oninput=e=>{ st.spr=e.target.value/100; send(); };
$('#dty').oninput=e=>{ st.dty=e.target.value/100; send(); };
$('#col').oninput=e=>{ st.col=e.target.value.replace('#',''); send(); };
$$('#pals input').forEach((el,i)=>{
  el.oninput=e=>{ st.p[i]=e.target.value.replace('#',''); send(); }
});

const taps = [];
$('#tapBtn').onclick = (e) => {
    e.target.style.transform = "scale(0.9)";
    setTimeout(()=>e.target.style.transform = "scale(1)", 100);

    const t = performance.now();
    if (taps.length > 0 && (t - taps[taps.length-1]) > 2000) taps.length = 0;
    
    taps.push(t);
    while (taps.length > 5) taps.shift(); 

    if (taps.length >= 2) {
        const diffs = taps.slice(1).map((x, i) => x - taps[i]);
        const avg = diffs.reduce((a, b) => a + b, 0) / diffs.length;
        let bpm = Math.round(60000 / avg);
        bpm = Math.max(10, Math.min(200, bpm)); 

        $('#bpm').value = bpm;
        $('#bpmVal').innerText = bpm;
        st.bpm = bpm;
        send();
    }
};
</script></body></html>
)rawliteral";

// Android Captive Portal 處理
void handleCaptivePortal() {
  String host = server.hostHeader();
  if (host != "192.168.4.1" || server.uri().indexOf("generate_204") >= 0) {
    server.sendHeader("Location", "http://192.168.4.1", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(200, "text/html", html_page);
}

void handleSet() {
  if (server.hasArg("m")) web_state.mode = server.arg("m").toInt();
  if (server.hasArg("bri")) web_state.brightness = server.arg("bri").toInt();
  if (server.hasArg("bpm")) web_state.bpm = server.arg("bpm").toInt();
  if (server.hasArg("c")) web_state.color = parseHex(server.arg("c"));
  if (server.hasArg("spd")) web_state.speed = server.arg("spd").toFloat();
  if (server.hasArg("spr")) web_state.spread = server.arg("spr").toFloat();
  if (server.hasArg("dty")) web_state.duty = server.arg("dty").toFloat();
  // 更新 Web 狀態的色盤
  if (server.hasArg("pal")) {
    String pStr = server.arg("pal");
    int lastIndex = 0;
    for(int i=0; i<4; i++) {
      int nextIndex = pStr.indexOf(',', lastIndex);
      if(nextIndex == -1) nextIndex = pStr.length();
      web_state.pal[i] = parseHex(pStr.substring(lastIndex, nextIndex));
      lastIndex = nextIndex + 1;
    }
  }
  server.send(200, "text/plain", "OK");
}

void setup() {
  Serial.begin(115200);
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(255); 
  WiFi.mode(WIFI_AP_STA); 
  WiFi.softAPConfig(apIP, apIP, IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, "", AP_CHANNEL, 0); 
  
  if (esp_now_init() != 0) return;
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(OnDataRecv); 
  
  dnsServer.start(DNS_PORT, "*", apIP);
  
  server.on("/generate_204", handleCaptivePortal);
  server.on("/gen_204", handleCaptivePortal);
  server.on("/chat", handleCaptivePortal);
  server.on("/", handleCaptivePortal);
  server.on("/set", handleSet);
  server.onNotFound(handleCaptivePortal);
  
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  if (millis() - last_server_time < 5000) {
    if (!is_server_online) is_server_online = true;
    render(&server_state, true); // True: 使用 Master 同步時間
  } else {
    if (is_server_online) is_server_online = false;
    render(&web_state, false);   // False: 使用本地時間
  }
  FastLED.show();
  delay(15);
}