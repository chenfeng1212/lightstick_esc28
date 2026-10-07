// =====================================================================
//  ESC LightStick — 終端燈具節點 v3
//  - v3：配合 Master v3 —— 指定時間同步切換 (applyAt)、淡入混色 (fadeMs)、
//        狀態版本 (seq) 去重，心跳重送不會重新觸發淡入
//  - 新版舞台風手機介面（燈光 / 設定 兩分頁）
//  - 可在手機頁修改 Wi-Fi 名稱、密碼、群組 ID（存在 EEPROM，重開機保留）
//  - HTML 存放在 PROGMEM，節省 RAM
//  - 修正：AP IP 與 Captive Portal 轉址 IP 不一致的問題
// =====================================================================
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <FastLED.h>
#include <espnow.h>

#define FW_VERSION  "v3.0"
#define PROTO_VER   3      // 必須與 Master / Gateway 相同

#define LED_PIN     D5
#define NUM_LEDS    4
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

// ---------- 出廠預設值（第一次開機時寫入 EEPROM） ----------
#define DEFAULT_SSID   "LightStick_001"
#define DEFAULT_PASS   ""          // 空字串 = 開放網路；若要設定須為 8–63 字元
#define DEFAULT_GROUP  9

// ESP-NOW 必須和 Master / Gateway 在同一個頻道，請勿修改
const int AP_CHANNEL = 1;

// ---------- EEPROM 設定 ----------
#define CFG_MAGIC 0x4C534332UL   // "LSC2"；改變這個值會強制重設成預設值

struct Config {
  uint32_t magic;
  char     ssid[33];   // 最多 32 bytes + '\0'
  char     pass[64];   // 最多 63 bytes + '\0'
  uint8_t  group;      // 1–10
};
Config cfg;

void loadConfig() {
  EEPROM.begin(sizeof(Config));
  EEPROM.get(0, cfg);
  bool valid = cfg.magic == CFG_MAGIC
            && cfg.ssid[32] == '\0' && cfg.pass[63] == '\0'
            && strlen(cfg.ssid) >= 1
            && cfg.group >= 1 && cfg.group <= 10;
  if (!valid) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CFG_MAGIC;
    strncpy(cfg.ssid, DEFAULT_SSID, 32);
    strncpy(cfg.pass, DEFAULT_PASS, 63);
    cfg.group = DEFAULT_GROUP;
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println("Config: 使用預設值");
  }
}

void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------- 燈光狀態 ----------
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

State web_state    = {0, 200, 120, 0x00CCFF, 1.2f, 0.6f, 0.5f, {0xFF0044, 0xFFD400, 0x00E5FF, 0xFFFFFF}};

// ===================== 封包（Master / Gateway / 手燈 必須完全相同） =====================
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

// 收到的封包先放進小佇列，在 loop() 中處理（callback 內不做複雜運算）
#define RXQ 8
Packet rxQueue[RXQ];
volatile uint8_t rxHead = 0, rxTail = 0;

// 現場狀態：fromState →（淡入）→ curState；pending 等待 applyAt
State    fromState, curState, pendState;
uint16_t curSeq = 0, pendSeq = 0;
bool     hasCur = false, hasPending = false;
uint32_t pendApplyAt = 0, pendFade = 0;
uint32_t fadeStart = 0, fadeMs = 0;

CRGB leds[NUM_LEDS];
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);   // v1 是 .2，但轉址到 .1，導致部分手機打不開頁面
DNSServer dnsServer;
ESP8266WebServer server(80);

volatile unsigned long last_server_time = 0;
bool is_server_online = false;
long timeOffset = 0;

unsigned long apRestartAt = 0;    // >0 時，到時間會用新設定重啟 AP

uint32_t parseHex(String s) { s.replace("#", ""); return strtoul(s.c_str(), NULL, 16); }
CRGB lerpColor(uint32_t c1, uint32_t c2, float f) { CRGB a(c1), b(c2); return blend(a, b, f * 255); }

void OnDataRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(Packet) || data[0] != PROTO_VER) return;   // 舊版 Master 的封包直接忽略
  uint8_t next = (rxHead + 1) % RXQ;
  if (next == rxTail) return;                                   // 佇列滿了就丟掉（心跳會補）
  memcpy(&rxQueue[rxHead], data, sizeof(Packet));
  rxHead = next;
}

uint32_t masterNow() { return millis() + timeOffset; }

void processPackets() {
  while (rxTail != rxHead) {
    Packet &p = rxQueue[rxTail];
    last_server_time = millis();
    timeOffset = (long)(p.timestamp - millis());     // 任何封包都可以用來校時

    if (p.targetGroup == 0 || p.targetGroup == cfg.group) {
      bool known = (hasCur && p.seq == curSeq) || (hasPending && p.seq == pendSeq);
      if (!known) {
        pendState.mode       = p.mode;
        pendState.brightness = p.brightness;
        pendState.bpm        = p.bpm;
        pendState.color      = p.color;
        pendState.speed      = p.speed;
        pendState.spread     = p.spread;
        pendState.duty       = p.duty;
        for (int i = 0; i < 4; i++) pendState.pal[i] = p.pal[i];
        pendSeq = p.seq;
        pendApplyAt = p.applyAt;
        pendFade = p.fadeMs;
        hasPending = true;
      }
    }
    rxTail = (rxTail + 1) % RXQ;
  }

  // 到了指定時間才切換 → 所有群組同一瞬間變化
  if (hasPending && (int32_t)(masterNow() - pendApplyAt) >= 0) {
    fromState = hasCur ? curState : pendState;
    curState  = pendState;
    curSeq    = pendSeq;
    fadeStart = pendApplyAt;          // 以 Master 時間計算，晚開機的手燈也會停在正確進度
    fadeMs    = hasCur ? pendFade : 0;
    hasCur = true;
    hasPending = false;
  }
}

// 把狀態畫到 out[]（含亮度），now 為毫秒時間
void render(const State *st, uint32_t now, CRGB *out) {
  CRGB *leds = out;

  uint32_t color = st->color;
  uint16_t bpm   = st->bpm;
  float speed    = st->speed;
  float spread   = st->spread;
  float duty     = st->duty;

  switch (st->mode) {
    case 0: fill_solid(leds, NUM_LEDS, CRGB::Black); break;
    case 1: fill_solid(leds, NUM_LEDS, CRGB(color)); break;
    case 2: { // 呼吸
        float ph = 2.0f * 3.1415926f * (bpm / 60000.0f) * now;
        float w = (cosf(ph) * 0.5f + 0.5f);
        CRGB c(color); c.nscale8(w * 255);
        fill_solid(leds, NUM_LEDS, c);
      } break;
    case 3: { // 彩虹
        float base = now * 0.001f * max(0.01f, speed);
        for (int i = 0; i < NUM_LEDS; i++) {
          float hue = fmodf(base + (i * spread) / (float)NUM_LEDS, 1.0f);
          leds[i] = CHSV(hue * 255, 255, 255);
        }
      } break;
    case 4: { // 窗口
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
    case 5: { // 節拍
        float beat = fmodf((now / 60000.0f) * bpm, 1.0f);
        bool on = beat <= constrain(duty, 0.05f, 0.95f);
        CRGB c(color); if (!on) c.nscale8(13);
        fill_solid(leds, NUM_LEDS, c);
      } break;
    case 7: { // 漸變
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
  nscale8_video(out, NUM_LEDS, st->brightness);
}

CRGB fadeBuf[NUM_LEDS];

void renderServer() {
  uint32_t now = masterNow();
  if (!hasCur) { fill_solid(leds, NUM_LEDS, CRGB::Black); return; }
  render(&curState, now, leds);
  if (fadeMs > 0) {
    int32_t el = (int32_t)(now - fadeStart);
    if (el < (int32_t)fadeMs) {
      render(&fromState, now, fadeBuf);
      uint8_t k = el <= 0 ? 0 : (uint8_t)((uint32_t)el * 255 / fadeMs);
      for (int i = 0; i < NUM_LEDS; i++) leds[i] = blend(fadeBuf[i], leds[i], k);
    } else {
      fadeMs = 0;
    }
  }
}

// ===================== 手機介面 HTML（存在 Flash） =====================
static const char html_page[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="zh-Hant">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"/>
<meta name="theme-color" content="#0A0A0D"/>
<title>手燈控制</title>
<style>
:root{
  --bg:#0A0A0D;--panel:#141419;--panel2:#1B1B22;--line:#24242C;
  --tx:#F2F2F5;--tx2:#8A8A96;--red:#FF3B4A;--ok:#3DDC97;
  --c:#00ccff;--r:16px;
  --mono:ui-monospace,"SF Mono",Menlo,Consolas,monospace;
}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;background:var(--bg);color:var(--tx);
  font:16px/1.5 -apple-system,"PingFang TC","Noto Sans TC","Microsoft JhengHei",sans-serif;user-select:none}
.wrap{max-width:480px;margin:0 auto;padding:16px 16px 40px}

/* ---------- header ---------- */
.top{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}
.brand{font-weight:800;letter-spacing:.04em;font-size:15px}
.brand small{display:block;color:var(--tx2);font:500 12px var(--mono);letter-spacing:0}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--tx2);
  padding:6px 10px;border:1px solid var(--line);border-radius:99px}
.pill i{width:7px;height:7px;border-radius:50%;background:var(--tx2)}
.pill.sync i{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.pill.sync{color:var(--tx)}

/* ---------- stage (hero) ---------- */
.stage{position:relative;height:230px;display:grid;place-items:center;margin:4px 0 12px}
.orb{width:150px;height:150px;border-radius:50%;background:var(--o,#000);
  box-shadow:0 0 60px 10px var(--o,#000),0 0 140px 30px color-mix(in srgb,var(--o,#000) 45%,transparent);
  transition:background .08s linear}
.stage::before{content:"";position:absolute;inset:0;
  background:radial-gradient(circle at 50% 50%,color-mix(in srgb,var(--o,#000) 18%,transparent),transparent 65%);pointer-events:none}
.leds{position:absolute;bottom:6px;display:flex;gap:14px}
.leds b{width:12px;height:12px;border-radius:50%;background:#000;border:1px solid var(--line)}
.notice{background:var(--panel);border:1px solid var(--line);border-radius:12px;
  padding:10px 12px;font-size:13px;color:var(--tx2);margin-bottom:12px;display:none}
.notice.show{display:block}

/* ---------- tabs ---------- */
.tabs{display:grid;grid-template-columns:1fr 1fr;background:var(--panel);border:1px solid var(--line);
  border-radius:99px;padding:4px;margin-bottom:16px}
.tabs button{all:unset;text-align:center;padding:10px;border-radius:99px;font-weight:700;font-size:15px;color:var(--tx2);cursor:pointer;transition:.2s}
.tabs button.on{background:var(--panel2);color:var(--tx)}
.view{display:none}.view.on{display:block;animation:in .25s ease-out}
@keyframes in{from{opacity:0;transform:translateY(6px)}}

/* ---------- modes ---------- */
.modes{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:16px}
.m{all:unset;text-align:center;min-height:52px;border-radius:99px;background:var(--panel);
  border:1px solid var(--line);font-weight:700;cursor:pointer;transition:.2s}
.m:active{transform:scale(.96)}
.m.on{background:var(--c);color:#000;border-color:transparent;
  box-shadow:0 0 24px color-mix(in srgb,var(--c) 60%,transparent)}
.m.off{grid-column:span 3;color:var(--red);border-color:color-mix(in srgb,var(--red) 40%,transparent)}
.m.off.on{background:var(--red);color:#fff;box-shadow:0 0 24px color-mix(in srgb,var(--red) 50%,transparent)}

/* ---------- panel / rows ---------- */
.panel{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:6px 16px;margin-bottom:12px}
.row{display:grid;grid-template-columns:52px 1fr 44px;gap:12px;align-items:center;min-height:56px;border-bottom:1px solid var(--line)}
.row:last-child{border-bottom:0}
.row[hidden]{display:none}
.row label{font-size:14px;color:var(--tx2)}
.row .v{font:600 14px var(--mono);text-align:right}
.row.bpm{grid-template-columns:52px 1fr 44px 64px}
input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:28px;background:transparent;cursor:pointer}
input[type=range]::-webkit-slider-runnable-track{height:6px;border-radius:6px;
  background:linear-gradient(90deg,var(--c) var(--p,50%),var(--line) var(--p,50%))}
input[type=range]::-moz-range-track{height:6px;border-radius:6px;background:var(--line)}
input[type=range]::-moz-range-progress{height:6px;border-radius:6px;background:var(--c)}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;margin-top:-9px;border-radius:50%;background:#fff;border:0;box-shadow:0 2px 8px #0008}
input[type=range]::-moz-range-thumb{width:24px;height:24px;border-radius:50%;background:#fff;border:0}
.tap{all:unset;text-align:center;height:40px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);
  font:800 13px var(--mono);letter-spacing:.08em;cursor:pointer;transition:transform .08s}
.tap.hit{transform:scale(.9);background:var(--c);color:#000}

/* colors */
.swatches{display:flex;gap:10px;flex-wrap:wrap}
.sw{position:relative;width:44px;height:44px;border-radius:50%;border:2px solid var(--line);overflow:hidden;cursor:pointer}
.sw input{position:absolute;inset:-10px;width:70px;height:70px;border:0;padding:0;opacity:0;cursor:pointer}
.sw.cur{border-color:#fff}
.row.col{grid-template-columns:52px 1fr}

/* ---------- settings ---------- */
.field{padding:12px 0;border-bottom:1px solid var(--line)}
.field:last-child{border-bottom:0}
.field label{display:block;font-size:13px;color:var(--tx2);margin-bottom:6px}
.field input[type=text],.field input[type=password]{width:100%;background:var(--bg);border:1px solid var(--line);border-radius:12px;
  color:var(--tx);font:16px var(--mono);padding:12px 44px 12px 14px;outline:none;transition:border-color .2s}
.field input:focus{border-color:var(--c)}
.field input.bad{border-color:var(--red)}
.field .hint{font-size:12px;color:var(--tx2);margin-top:6px}
.field .hint.bad{color:var(--red)}
.pw{position:relative}
.eye{all:unset;position:absolute;right:6px;top:50%;transform:translateY(-50%);padding:8px;font-size:12px;color:var(--tx2);cursor:pointer}
.groups{display:grid;grid-template-columns:repeat(5,1fr);gap:6px}
.groups button{all:unset;text-align:center;height:44px;border-radius:10px;background:var(--bg);border:1px solid var(--line);font:700 15px var(--mono);cursor:pointer}
.groups button.on{background:var(--c);color:#000;border-color:transparent}
.warn{border:1px solid color-mix(in srgb,#FFB020 50%,transparent);background:color-mix(in srgb,#FFB020 8%,transparent);
  color:#FFCF70;border-radius:12px;padding:10px 12px;font-size:13px;margin-bottom:12px}
.save{all:unset;display:block;width:100%;text-align:center;height:54px;line-height:54px;border-radius:99px;
  background:var(--c);color:#000;font-weight:800;cursor:pointer;margin-top:4px;transition:.2s}
.save:disabled{opacity:.35;cursor:not-allowed}
.save:active:not(:disabled){transform:scale(.98)}
.meta{font:12px var(--mono);color:var(--tx2);text-align:center;margin-top:16px}

/* ---------- sheet ---------- */
.sheet{position:fixed;inset:0;background:#000a;display:none;align-items:flex-end;justify-content:center;z-index:9}
.sheet.on{display:flex}
.sheet .box{width:100%;max-width:480px;background:var(--panel);border-top-left-radius:24px;border-top-right-radius:24px;
  padding:24px 20px 32px;border:1px solid var(--line);animation:up .25s ease-out}
@keyframes up{from{transform:translateY(40px);opacity:0}}
.sheet h3{margin:0 0 8px;font-size:20px}
.sheet p{color:var(--tx2);margin:0 0 16px;font-size:14px}
.sheet .new{font:700 18px var(--mono);background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:12px;text-align:center;margin-bottom:16px;word-break:break-all}
.ghost{all:unset;display:block;width:100%;text-align:center;height:48px;line-height:48px;color:var(--tx2);cursor:pointer;margin-top:8px}

.toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,20px);opacity:0;pointer-events:none;
  background:var(--panel2);border:1px solid var(--line);border-radius:99px;padding:10px 18px;font-size:14px;transition:.25s;z-index:10}
.toast.on{opacity:1;transform:translate(-50%,0)}

@media (prefers-reduced-motion:reduce){*{animation:none!important;transition:none!important}}
</style>
</head>
<body>
<div class="wrap">

  <div class="top">
    <div class="brand">ESC LIGHTSTICK<small id="hdrSsid">—</small></div>
    <div class="pill" id="pill"><i></i><span>獨立模式</span></div>
  </div>

  <div class="stage">
    <div class="orb" id="orb"></div>
    <div class="leds" id="leds"><b></b><b></b><b></b><b></b></div>
  </div>

  <div class="notice" id="syncNote">目前正在接收中控台訊號，這裡的調整會在中控停止 5 秒後才生效。</div>

  <div class="tabs">
    <button class="on" data-v="light">燈光</button>
    <button data-v="set">設定</button>
  </div>

  <!-- ================= 燈光 ================= -->
  <section class="view on" id="v-light">
    <div class="modes">
      <button class="m" data-m="1">恆亮</button>
      <button class="m on" data-m="2">呼吸</button>
      <button class="m" data-m="3">彩虹</button>
      <button class="m" data-m="4">窗口</button>
      <button class="m" data-m="5">節拍</button>
      <button class="m" data-m="7">漸變</button>
      <button class="m off" data-m="0">關閉</button>
    </div>

    <div class="panel">
      <div class="row"><label>亮度</label><input id="bri" type="range" min="0" max="255" value="200"><span class="v" id="briV">78%</span></div>
      <div class="row bpm" data-for="2 5"><label>BPM</label><input id="bpm" type="range" min="10" max="200" value="120"><span class="v" id="bpmV">120</span><button class="tap" id="tap">TAP</button></div>
      <div class="row" data-for="3 4 7"><label>速度</label><input id="spd" type="range" min="0" max="300" value="120"><span class="v" id="spdV">1.2</span></div>
      <div class="row" data-for="3 4"><label>展開</label><input id="spr" type="range" min="0" max="100" value="60"><span class="v" id="sprV">60</span></div>
      <div class="row" data-for="5"><label>亮佔比</label><input id="dty" type="range" min="5" max="95" value="50"><span class="v" id="dtyV">50%</span></div>
      <div class="row col" data-for="1 2 4 5"><label>主色</label>
        <div class="swatches" id="mainSw"></div></div>
      <div class="row col" data-for="7"><label>色盤</label>
        <div class="swatches" id="palSw"></div></div>
    </div>
  </section>

  <!-- ================= 設定 ================= -->
  <section class="view" id="v-set">
    <div class="warn" id="openWarn">此手燈目前沒有設定密碼，任何人都能連線並修改。建議設定一組密碼。</div>

    <div class="panel">
      <div class="field">
        <label for="ssid">Wi-Fi 名稱 (SSID)</label>
        <input type="text" id="ssid" maxlength="32" autocomplete="off" spellcheck="false">
        <div class="hint" id="ssidH">1–32 個字元</div>
      </div>
      <div class="field">
        <label for="npw">新密碼</label>
        <div class="pw"><input type="password" id="npw" maxlength="63" autocomplete="new-password" placeholder="留空 = 不變更"><button class="eye" data-t="npw">顯示</button></div>
        <div class="hint" id="npwH">8–63 個字元；留空表示維持目前密碼</div>
      </div>
      <div class="field">
        <label>群組</label>
        <div class="groups" id="groups"></div>
      </div>
    </div>

    <div class="panel" id="curBox">
      <div class="field">
        <label for="cpw">目前密碼</label>
        <div class="pw"><input type="password" id="cpw" maxlength="63" autocomplete="current-password"><button class="eye" data-t="cpw">顯示</button></div>
        <div class="hint" id="cpwH">儲存前需驗證目前密碼</div>
      </div>
    </div>

    <button class="save" id="save" disabled>儲存設定</button>
    <div class="meta" id="meta">—</div>
  </section>
</div>

<div class="toast" id="toast"></div>

<!-- confirm sheet -->
<div class="sheet" id="sheet"><div class="box">
  <h3>確認套用？</h3>
  <p>儲存後手燈會重新啟動 Wi-Fi，你的手機將會斷線。請到手機 Wi-Fi 設定重新連線到：</p>
  <div class="new" id="newSsid">—</div>
  <button class="save" id="go">確認並重新啟動</button>
  <button class="ghost" id="cancel">取消</button>
</div></div>

<!-- done sheet -->
<div class="sheet" id="done"><div class="box">
  <h3>已儲存</h3>
  <p>手燈正在重新啟動 Wi-Fi。請連線到下方網路，連上後會自動回到此頁面。</p>
  <div class="new" id="doneSsid">—</div>
</div></div>

<script>
const $=s=>document.querySelector(s),$$=s=>document.querySelectorAll(s);
const PRESET=['00ccff','ff0044','ffd400','3ddc97','b14cff','ffffff'];
let st={m:2,bri:200,bpm:120,col:'00ccff',spd:1.2,spr:.6,dty:.5,p:['ff0044','ffd400','00e5ff','ffffff']};
let cfg={ssid:'LightStick_140',group:9,hasPw:false,online:false,mac:'—',fw:'—'};
let selGroup=9;

/* ---------- API（在 ESP8266 外預覽時自動使用假資料） ---------- */
const MOCK=!/^192\.168\.4\./.test(location.hostname);
// 節流：拖動滑桿時最多每 80ms 送一次，並保證最後一個值一定送出（避免 ESP8266 被請求塞爆）
let sendT=0,sendPend=false;
function send(){
  if(sendT){sendPend=true;return;}
  doSend();
  sendT=setTimeout(function f(){if(sendPend){sendPend=false;doSend();sendT=setTimeout(f,80);}else sendT=0;},80);
}
function doSend(){
  const u=`/set?m=${st.m}&bri=${st.bri}&bpm=${st.bpm}&c=${st.col}&spd=${st.spd}&spr=${st.spr}&dty=${st.dty}&pal=${st.p.join(',')}`;
  if(!MOCK)fetch(u).catch(()=>{});
}
async function getCfg(){
  if(MOCK)return {ssid:'LightStick_140',group:9,hasPw:false,online:false,mac:'5C:CF:7F:XX:XX:XX',fw:'preview'};
  return await (await fetch('/config')).json();
}
function showStatus(){
  $('#pill').classList.toggle('sync',cfg.online);
  $('#pill span').textContent=cfg.online?'中控同步中':'獨立模式';
  $('#syncNote').classList.toggle('show',cfg.online);
}
async function loadCfg(){
  try{cfg=await getCfg()}catch(e){}
  if(cfg.st){ // 用手燈目前的狀態初始化介面
    const s=cfg.st;Object.assign(st,{m:s.m,bri:s.bri,bpm:s.bpm,col:s.col,spd:s.spd,spr:s.spr,dty:s.dty,p:s.p});
    const ui={bri:st.bri,bpm:st.bpm,spd:Math.round(st.spd*100),spr:Math.round(st.spr*100),dty:Math.round(st.dty*100)};
    for(const k in ui){const el=$('#'+k);el.value=ui[k];$('#'+k+'V').textContent=SL[k][1](+el.value);fill(el);}
    setAccent();drawMain();drawPal();applyMode();
  }
  selGroup=cfg.group;
  $('#ssid').value=cfg.ssid;$('#hdrSsid').textContent=cfg.ssid;
  $('#openWarn').style.display=cfg.hasPw?'none':'block';
  $('#curBox').style.display=cfg.hasPw?'block':'none';
  $('#meta').textContent=`G${cfg.group} · ${cfg.mac} · ${cfg.fw}`;
  showStatus();drawGroups();validate();
}
// 每 3 秒更新一次「中控同步中 / 獨立模式」
setInterval(async()=>{if(MOCK||document.hidden)return;try{const c=await getCfg();cfg.online=c.online;showStatus();}catch(e){}},3000);
async function saveCfg(){
  const body=new URLSearchParams({ssid:$('#ssid').value.trim(),npw:$('#npw').value,group:selGroup,cpw:$('#cpw').value});
  if(MOCK)return {ok:true,restart:$('#ssid').value.trim()!==cfg.ssid||$('#npw').value!==''};
  try{const r=await fetch('/config',{method:'POST',body});return await r.json()}catch(e){return {ok:false,err:'連線失敗，請再試一次'}}
}
function wifiChanged(){return $('#ssid').value.trim()!==cfg.ssid||$('#npw').value!==''}

/* ---------- tabs ---------- */
$$('.tabs button').forEach(b=>b.onclick=()=>{
  $$('.tabs button').forEach(x=>x.classList.toggle('on',x===b));
  $$('.view').forEach(v=>v.classList.toggle('on',v.id==='v-'+b.dataset.v));
});

/* ---------- modes ---------- */
function applyMode(){
  $$('.m').forEach(b=>b.classList.toggle('on',+b.dataset.m===+st.m));
  $$('.row[data-for]').forEach(r=>r.hidden=!r.dataset.for.split(' ').includes(String(st.m)));
}
$$('.m').forEach(b=>b.onclick=()=>{st.m=+b.dataset.m;applyMode();send();});

/* ---------- sliders ---------- */
function fill(el){el.style.setProperty('--p',((el.value-el.min)/(el.max-el.min)*100)+'%')}
const SL={
  bri:[v=>st.bri=v,v=>Math.round(v/2.55)+'%'],
  bpm:[v=>st.bpm=v,v=>v],
  spd:[v=>st.spd=v/100,v=>(v/100).toFixed(1)],
  spr:[v=>st.spr=v/100,v=>v],
  dty:[v=>st.dty=v/100,v=>v+'%'],
};
for(const k in SL){const el=$('#'+k);fill(el);el.oninput=()=>{SL[k][0](+el.value);$('#'+k+'V').textContent=SL[k][1](+el.value);fill(el);send();}}

/* ---------- tap tempo ---------- */
const taps=[];
$('#tap').onclick=e=>{
  const b=e.currentTarget;b.classList.add('hit');setTimeout(()=>b.classList.remove('hit'),100);
  const t=performance.now();
  if(taps.length&&t-taps[taps.length-1]>2000)taps.length=0;
  taps.push(t);while(taps.length>5)taps.shift();
  if(taps.length>=2){
    const d=taps.slice(1).map((x,i)=>x-taps[i]);
    let bpm=Math.max(10,Math.min(200,Math.round(60000/(d.reduce((a,b)=>a+b)/d.length))));
    $('#bpm').value=bpm;$('#bpm').oninput();
  }
};

/* ---------- colors ---------- */
function setAccent(){document.documentElement.style.setProperty('--c','#'+st.col)}
function swatch(hex,onPick,cur){
  const d=document.createElement('label');d.className='sw'+(cur?' cur':'');d.style.background='#'+hex;
  const i=document.createElement('input');i.type='color';i.value='#'+hex;
  i.oninput=()=>{d.style.background=i.value;onPick(i.value.slice(1));};
  d.appendChild(i);return d;
}
function drawMain(){
  const w=$('#mainSw');w.innerHTML='';
  PRESET.forEach(h=>{const s=document.createElement('button');s.className='sw'+(h===st.col?' cur':'');s.style.background='#'+h;
    s.onclick=()=>{st.col=h;setAccent();drawMain();send();};w.appendChild(s);});
  // 自訂顏色
  const custom=!PRESET.includes(st.col);
  const c=swatch(custom?st.col:'888888',h=>{st.col=h;setAccent();send();},custom);
  c.style.background=custom?'#'+st.col:'conic-gradient(red,yellow,lime,cyan,blue,magenta,red)';
  c.title='自訂';w.appendChild(c);
}
function drawPal(){const w=$('#palSw');w.innerHTML='';st.p.forEach((h,i)=>w.appendChild(swatch(h,v=>{st.p[i]=v;send();})))}

/* ---------- 燈色預覽：與韌體 render() 相同的公式 ---------- */
const hex2=h=>[parseInt(h.slice(0,2),16),parseInt(h.slice(2,4),16),parseInt(h.slice(4,6),16)];
const sc=(c,k)=>c.map(x=>x*k);
const hsv=h=>{const f=(n,k=(n+h*6)%6)=>255*(1-Math.max(Math.min(k,4-k,1),0));return[f(5),f(3),f(1)]};
const lerp=(a,b,f)=>a.map((x,i)=>x+(b[i]-x)*f);
function frame(now){
  const N=4,out=[];const c=hex2(st.col);
  for(let i=0;i<N;i++){
    let v=[0,0,0];
    switch(st.m){
      case 1:v=c;break;
      case 2:v=sc(c,Math.cos(2*Math.PI*(st.bpm/60000)*now)*.5+.5);break;
      case 3:v=hsv(((now*.001*Math.max(.01,st.spd))+i*st.spr/N)%1);break;
      case 4:{const p=(now*.001*Math.max(.01,st.spd))%1,h=Math.min(1,Math.max(.05,st.spr))/2;v=sc(c,Math.abs(i/(N-1)-p)<h?1:.1)}break;
      case 5:v=sc(c,((now/60000)*st.bpm)%1<=st.dty?1:.05);break;
      case 7:{const p=((now*.001*Math.max(.01,st.spd))%1)*4,a=Math.floor(p)%4;v=lerp(hex2(st.p[a]),hex2(st.p[(a+1)%4]),p-Math.floor(p))}break;
    }
    out.push(sc(v,st.bri/255));
  }
  const css=a=>`rgb(${a.map(Math.round).join(',')})`;
  $$('#leds b').forEach((b,i)=>{b.style.background=css(out[i]);b.style.boxShadow=`0 0 10px ${css(out[i])}`});
  const avg=[0,1,2].map(k=>out.reduce((s,o)=>s+o[k],0)/N);
  $('#orb').style.setProperty('--o',css(avg));
  requestAnimationFrame(frame);
}

/* ---------- 設定驗證 ---------- */
function drawGroups(){
  const g=$('#groups');g.innerHTML='';
  for(let i=1;i<=10;i++){const b=document.createElement('button');b.textContent=i;b.className=i===selGroup?'on':'';
    b.onclick=()=>{selGroup=i;drawGroups();validate();};g.appendChild(b);}
}
function validate(){
  const s=$('#ssid').value.trim(),n=$('#npw').value,c=$('#cpw').value;
  const sOk=s.length>=1&&new TextEncoder().encode(s).length<=32;
  const nOk=n===''||(n.length>=8&&n.length<=63);
  const cOk=!cfg.hasPw||c.length>=8;
  const changed=s!==cfg.ssid||n!==''||selGroup!==cfg.group;
  $('#ssid').classList.toggle('bad',!sOk);$('#ssidH').classList.toggle('bad',!sOk);
  $('#npw').classList.toggle('bad',!nOk);$('#npwH').classList.toggle('bad',!nOk);
  $('#npwH').textContent=nOk?'8–63 個字元；留空表示維持目前密碼':`還差 ${Math.max(0,8-n.length)} 個字元（最少 8 個）`;
  $('#save').disabled=!(sOk&&nOk&&cOk&&changed);
}
['ssid','npw','cpw'].forEach(id=>$('#'+id).oninput=validate);
$$('.eye').forEach(b=>b.onclick=()=>{const i=$('#'+b.dataset.t);i.type=i.type==='password'?'text':'password';b.textContent=i.type==='password'?'顯示':'隱藏';});

$('#cpw').addEventListener('input',()=>{$('#cpw').classList.remove('bad');$('#cpwH').classList.remove('bad');$('#cpwH').textContent='儲存前需驗證目前密碼';});
function toast(t){const e=$('#toast');e.textContent=t;e.classList.add('on');clearTimeout(e._t);e._t=setTimeout(()=>e.classList.remove('on'),2200);}
function fail(r){
  if(r.field==='cpw'||!r.field){$('#cpw').classList.add('bad');$('#cpwH').classList.add('bad');$('#cpwH').textContent=r.err||'目前密碼錯誤';}
  toast(r.err||'儲存失敗');
}
async function commit(btn,label){
  btn.disabled=true;const old=btn.textContent;btn.textContent=label;
  const r=await saveCfg();
  btn.disabled=false;btn.textContent=old;
  return r;
}
$('#save').onclick=async()=>{
  if(wifiChanged()){$('#newSsid').textContent=$('#ssid').value.trim();$('#sheet').classList.add('on');return;}
  // 只改群組：不需要重啟 Wi-Fi
  const r=await commit($('#save'),'儲存中…');
  if(r.ok){cfg.group=selGroup;$('#cpw').value='';$('#meta').textContent=`G${cfg.group} · ${cfg.mac} · ${cfg.fw}`;validate();toast(`已切換到群組 ${selGroup}`);}
  else{fail(r);validate();}
};
$('#cancel').onclick=()=>$('#sheet').classList.remove('on');
$('#go').onclick=async()=>{
  const r=await commit($('#go'),'儲存中…');
  $('#sheet').classList.remove('on');
  if(r.ok){$('#doneSsid').textContent=$('#ssid').value.trim();$('#done').classList.add('on');}
  else fail(r);
};

/* ---------- init ---------- */
setAccent();drawMain();drawPal();applyMode();loadCfg();requestAnimationFrame(frame);
</script>
</body>
</html>
)rawliteral";

// ===================== HTTP handlers =====================
void handleCaptivePortal() {
  String host = server.hostHeader();
  if (host != apIP.toString() || server.uri().indexOf("generate_204") >= 0) {
    server.sendHeader("Location", String("http://") + apIP.toString(), true);
    server.send(302, "text/plain", "");
    return;
  }
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", html_page);
}

void handleSet() {
  if (server.hasArg("m"))   web_state.mode       = server.arg("m").toInt();
  if (server.hasArg("bri")) web_state.brightness = server.arg("bri").toInt();
  if (server.hasArg("bpm")) web_state.bpm        = server.arg("bpm").toInt();
  if (server.hasArg("c"))   web_state.color      = parseHex(server.arg("c"));
  if (server.hasArg("spd")) web_state.speed      = server.arg("spd").toFloat();
  if (server.hasArg("spr")) web_state.spread     = server.arg("spr").toFloat();
  if (server.hasArg("dty")) web_state.duty       = server.arg("dty").toFloat();
  if (server.hasArg("pal")) {
    String pStr = server.arg("pal");
    int lastIndex = 0;
    for (int i = 0; i < 4; i++) {
      int nextIndex = pStr.indexOf(',', lastIndex);
      if (nextIndex == -1) nextIndex = pStr.length();
      web_state.pal[i] = parseHex(pStr.substring(lastIndex, nextIndex));
      lastIndex = nextIndex + 1;
    }
  }
  server.send(200, "text/plain", "OK");
}

// JSON 字串跳脫（SSID 可能含有 " 或 \）
String jsonEsc(const char *s) {
  String o;
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[7]; snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c); o += b; }
    else o += c;
  }
  return o;
}

String hex6(uint32_t v) { char b[7]; snprintf(b, sizeof(b), "%06x", (unsigned)(v & 0xFFFFFF)); return String(b); }

// GET /config：回傳設定（不含密碼）、連線狀態、目前燈光狀態
void handleGetConfig() {
  String j;
  j.reserve(400);
  j  = "{\"ssid\":\"";  j += jsonEsc(cfg.ssid);
  j += "\",\"group\":"; j += cfg.group;
  j += ",\"hasPw\":";   j += (strlen(cfg.pass) > 0 ? "true" : "false");
  j += ",\"online\":";  j += (is_server_online ? "true" : "false");
  j += ",\"mac\":\"";   j += WiFi.softAPmacAddress();
  j += "\",\"fw\":\"" FW_VERSION "\"";
  j += ",\"st\":{\"m\":";  j += web_state.mode;
  j += ",\"bri\":";        j += web_state.brightness;
  j += ",\"bpm\":";        j += web_state.bpm;
  j += ",\"col\":\"";      j += hex6(web_state.color);
  j += "\",\"spd\":";      j += String(web_state.speed, 2);
  j += ",\"spr\":";        j += String(web_state.spread, 2);
  j += ",\"dty\":";        j += String(web_state.duty, 2);
  j += ",\"p\":[";
  for (int i = 0; i < 4; i++) { if (i) j += ','; j += '"'; j += hex6(web_state.pal[i]); j += '"'; }
  j += "]}}";
  server.send(200, "application/json; charset=utf-8", j);
}

void sendErr(const char *field, const char *msg) {
  String j = String("{\"ok\":false,\"field\":\"") + field + "\",\"err\":\"" + msg + "\"}";
  server.send(400, "application/json; charset=utf-8", j);
}

// POST /config：ssid, npw(新密碼，可空), group, cpw(目前密碼)
void handlePostConfig() {
  String ssid = server.arg("ssid"); ssid.trim();
  String npw  = server.arg("npw");
  String cpw  = server.arg("cpw");
  int group   = server.hasArg("group") ? server.arg("group").toInt() : cfg.group;

  // 1. 驗證目前密碼（尚未設定密碼時略過）
  if (strlen(cfg.pass) > 0 && cpw != String(cfg.pass)) { sendErr("cpw", "目前密碼錯誤"); return; }
  // 2. 格式檢查（前端也有檢查，這裡是最後防線）
  if (ssid.length() < 1 || ssid.length() > 32)          { sendErr("ssid", "Wi-Fi 名稱需為 1–32 個字元"); return; }
  if (npw.length() > 0 && (npw.length() < 8 || npw.length() > 63)) { sendErr("npw", "密碼需為 8–63 個字元"); return; }
  if (group < 1 || group > 10)                           { sendErr("group", "群組需為 1–10"); return; }

  bool wifiChanged = (ssid != String(cfg.ssid)) || npw.length() > 0;

  strncpy(cfg.ssid, ssid.c_str(), 32); cfg.ssid[32] = '\0';
  if (npw.length() > 0) { strncpy(cfg.pass, npw.c_str(), 63); cfg.pass[63] = '\0'; }
  cfg.group = group;
  saveConfig();

  server.send(200, "application/json; charset=utf-8",
              wifiChanged ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");

  // 延遲 1 秒再重啟 AP，讓回應先送到手機
  if (wifiChanged) apRestartAt = millis() + 1000;
}

void startAP() {
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  // 空密碼 = 開放網路
  WiFi.softAP(cfg.ssid, strlen(cfg.pass) ? cfg.pass : NULL, AP_CHANNEL, 0);
  Serial.printf("AP: %s (group %d)\n", cfg.ssid, cfg.group);
}

void setup() {
  Serial.begin(115200);
  loadConfig();

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(255);   // 亮度改在 render() 內處理（淡入時兩種亮度才能正確混合）

  WiFi.mode(WIFI_AP_STA);
  startAP();

  if (esp_now_init() != 0) return;
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(OnDataRecv);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/generate_204", handleCaptivePortal);
  server.on("/gen_204", handleCaptivePortal);
  server.on("/chat", handleCaptivePortal);
  server.on("/", handleCaptivePortal);
  server.on("/set", handleSet);
  server.on("/config", HTTP_GET, handleGetConfig);
  server.on("/config", HTTP_POST, handlePostConfig);
  server.onNotFound(handleCaptivePortal);

  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  processPackets();

  if (apRestartAt && (long)(millis() - apRestartAt) >= 0) {
    apRestartAt = 0;
    WiFi.softAPdisconnect(false);
    startAP();   // 頻道不變，ESP-NOW 不受影響
  }

  is_server_online = last_server_time != 0 && (millis() - last_server_time < 5000);
  if (is_server_online) renderServer();
  else render(&web_state, millis(), leds);
  FastLED.show();
  delay(10);
}
