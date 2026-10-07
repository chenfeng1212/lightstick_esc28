# lightstick - ESP-NOW 無線燈光控制系統

## 專案概述 (Project Overview)
ESP-NOW 無線燈光控制系統是一套為演唱會與互動展演設計的高效能、低延遲分散式燈光控制解決方案。

不同於傳統基於 Wi-Fi 路由器的控制系統，本專案採用 ESP-NOW 無連線通訊協定 (Connectionless Communication Protocol)。此架構允許單一中央控制器在不依賴外部路由器的情況下，以毫秒級的精度同步控制數百個 LED 終端節點，徹底解決了大規模連線下的延遲與斷線問題。

## 系統架構 (System Architecture)
本系統採用星型與網狀混合拓撲 (Star-Mesh Hybrid Topology)，由以下四個核心組件構成：

1. 中央控制端 (Central Controller / PC Server):

    - 運行於電腦上的 Node.js 伺服器。

    - 提供圖形化網頁介面 (Web GUI)，用於即時控制、群組管理與模式排程。

    - 透過 USB 序列埠 (UART) 與主發射節點進行通訊。

2. 主發射節點 (Master Node):

    - 作為 USB 轉 ESP-NOW 的橋接器。

    - 負責將控制指令廣播至所有終端與中繼節點。

    - 實作「連發機制 (Burst Fire)」以降低封包丟失率。

    - 作為系統的主時鐘源 (Master Clock)，提供時間同步基準。

3. 中繼節點 (Gateway Nodes):

    - 選配組件，用於訊號範圍延伸。

    - 監聽主發射節點的廣播並進行訊號轉發 (Re-transmit)。

    - 具備隨機避讓機制 (Random Backoff) 與重複封包過濾功能，防止訊號碰撞。

4. 終端燈具節點 (Slave Nodes):

    - 可攜式 LED 控制器 (ESP8266 + WS2812B)。

    - 具備雙模運作機制：

      - 控制模式 (Control Mode): 接收 ESP-NOW 廣播指令進行同步運作。

      - 獨立模式 (Standalone Mode): 當訊號中斷時，自動切換至 Wi-Fi AP 模式並啟動 Captive Portal 供手機單獨控制。

    - 可透過手機頁面自行修改 Wi-Fi 名稱、密碼與群組 ID，設定儲存於 EEPROM，重新開機後仍保留。
## 核心功能 (Key Features)

- 極低延遲傳輸: 利用 OSI 模型第二層 (Layer 2) 的 ESP-NOW 協定，實現低於 10ms 的響應速度。

- 時間同步機制: 實作主從時間偏差校正演算法，確保所有裝置的動畫相位（如呼吸燈頻率）精確同步。

- 高可靠性: 採用「連發機制 (Burst Fire)」與心跳封包 (Heartbeat)，確保指令在充滿干擾的環境中仍能送達。

- 高擴充性: 支援群組控制（預設 10 個獨立群組 + 全域廣播）。

- 自動故障轉移 (Failover): 當接收不到主控訊號超過 5 秒，裝置自動進入獨立 AP 模式。

- Android 相容性優化: 針對 Android 系統優化 Captive Portal DNS 行為 (類 RFC 8908)，解決控制頁面無法彈出的問題。

- 手機自助設定: 使用者可在手燈頁面修改 Wi-Fi 名稱 (SSID)、密碼與群組 ID，無需重新燒錄。已設定密碼時須輸入目前密碼才能修改。

- 即時燈色預覽: 手機頁與電腦中控台皆以與韌體相同的公式即時模擬 4 顆 LED 的燈效。

- 預覽 → GO: 在中控台的預覽區調整時手燈完全不受影響，按下 GO 才一次送上現場，觀眾不會看到調整過程。需要即時跟拍時可切換 LIVE 模式。

- 多群組同步切換: 封包帶有「套用時間 (applyAt)」，所有群組在同一個 Master 時間點切換，不再有依序延遲。

- 場景: 將 10 個群組的狀態與淡入時間存成場景，演出時點一下即可切換。場景存在 `scenes.json`。

- 淡入: 每次 GO 或每個場景可設定 0–3 秒淡入，手燈會將新舊效果混色過渡。

- 心跳輪流補送: Master 記住 10 個群組的狀態並每 0.5 秒輪流補送，漏收或活動中才開機的手燈也會自動同步。
    
## 技術規格 (Technical Specifications)

### 硬體需求

- 微控制器: ESP8266 (NodeMCU v2/v3 或 Wemos D1 Mini) x 3+。

- 燈光設備: WS2812B / SK6812 可定址 LED 燈條。

- 電源供應: 5V 直流電源或行動電源。

- 連接介面: Micro-USB 傳輸線 (用於主發射節點)。

### 軟體技術棧

- 後端服務: Node.js, Express.js, SerialPort.

- 前端介面: HTML5, CSS3, Vanilla JavaScript (SPA 架構).

- 韌體開發: Arduino IDE (C++), FastLED Library, ESP8266WiFi, EEPROM.

- 通訊協定資料結構
 
### 系統透過 ESP-NOW 傳輸固定大小的 C Struct（協定版本 3，52 bytes）：

> Master、Gateway、手燈三者的 struct 必須完全相同。手燈會直接忽略 `ver` 不是 3 的封包（舊版韌體送出的封包）。

| 欄位名稱 | 類型 | 說明 |
|--------- | ------- | -------------------- |
| ver | uint8_t | 協定版本，目前為 3 |
| msgId | uint8_t | 傳輸序號，Gateway 用來過濾重複封包 |
| targetGroup | uint8_t | 目標群組 (0=全體, 1-10=特定群組) |
| mode | uint8_t | 運作模式 (0=關閉, 1=恆亮, 2=呼吸, 3=彩虹, 4=窗口, 5=節拍, 7=漸變) |
| brightness | uint8_t | 亮度 0–255 |
| reserved | uint8_t | 保留 |
| bpm | uint16_t | 節奏速率 (Beats Per Minute) |
| fadeMs | uint16_t | 淡入時間 (毫秒) |
| seq | uint16_t | 狀態版本；相同 seq 代表同一個狀態，手燈不會重新淡入 |
| color | uint32_t | 主色代碼 (HEX) |
| speed / spread / duty | float | 速度 / 展開 / 亮佔比 |
| pal[4] | uint32_t | 4 色漸變色盤 |
| timestamp | uint32_t | 送出當下的 Master 時間，用於校時 |
| applyAt | uint32_t | 在 Master 的哪個時間點套用，用於多群組同步切換 |

### Master 序列埠協定

每行一個指令，以 `\n` 結尾：

| 指令 | 說明 |
|------|------|
| `S,gid,mode,bri,bpm,color,spd,spr,dty,p1,p2,p3,p4` | 暫存群組狀態（gid 0 = 全部），不會立即送出 |
| `GO,fadeMs` | 把暫存的群組一次送出，並指定淡入時間 |
| `STOP` | 停止廣播與心跳 |
| `gid,mode,bri,…,p4` | 舊格式，相容保留（等同 `S` + `GO,0`） |

### 中控伺服器 API

| 方法 | 路徑 | 說明 |
|------|------|------|
| GET | `/api/ports` | 列出序列埠 |
| POST | `/api/connect` / `/api/disconnect` | 連線 / 斷線 |
| POST | `/api/go` | `{ fade, slots:[{ gid, mode, bri, bpm, col, spd, spr, dty, p:[4] }] }` 一次送出多個群組 |
| GET | `/api/program` | 目前現場狀態（重新整理頁面時用來還原） |
| GET / PUT | `/api/scenes` | 讀取 / 覆寫 `scenes.json` |
| POST | `/api/send` | 舊介面，單一群組立即送出 |

## 專案結構 (Project Structure)

```
lightstick_esc28/
├── README.md
├── package.json               # Node.js 相依套件
├── server.js                  # 中控伺服器
├── scenes.json                # 場景庫（第一次儲存場景時自動建立）
├── public/
│   ├── index.html             # 電腦中控台介面
│   └── style.css
└── firmware/
    ├── master/master.ino      # 主發射節點
    ├── gateway/gateway.ino    # 中繼節點（選配）
    └── lightstick/
        ├── lightstick.ino     # 終端燈具節點（含手機控制頁與 Wi-Fi 設定）
        └── phone_ui_preview.html  # 手機頁原始檔，可直接用瀏覽器預覽
```

> **注意：Master、Gateway、手燈必須燒錄同一版本的韌體**。封包格式不同時會收不到指令。舊版本請從 git 歷史取得。

## 安裝與部署 (Installation and Deployment)
1. 伺服器端設置 (PC Server)
請確保控制電腦已安裝 Node.js 環境。
```bash
# 安裝相依套件
npm install

# 啟動控制伺服器
node server.js
```

啟動後，請使用瀏覽器訪問 ```http://localhost:3000``` 進入控制儀表板。

2. 韌體燒錄 (Firmware Flashing)
使用 Arduino IDE 將對應的韌體燒錄至各 ESP8266 裝置。

    - 必要函式庫: FastLED, ESP8266WiFi.

    A. 主發射節點 (Master Node)
  
     1. 開啟 ```firmware/master/master.ino```。

     2. 燒錄至連接電腦的 ESP8266。

     3. 注意: 燒錄完成後，需在網頁介面選擇正確的 COM Port 進行連線。

    B. 終端燈具節點 (Slave Node)

     1. 開啟 ```firmware/lightstick/lightstick.ino```。

     2. 必要時修改硬體設定：

        - 根據硬體連接修改 `LED_PIN` 與 `NUM_LEDS`。

        - 若燈色錯誤，修改 `COLOR_ORDER`（常見為 RGB 或 GRB）。

     3. （選用）修改出廠預設值，僅在第一次開機時寫入：

        - `DEFAULT_SSID`：預設 Wi-Fi 名稱（預設 `LightStick_140`）

        - `DEFAULT_PASS`：預設密碼，空字串表示開放網路；若設定須為 8–63 字元

        - `DEFAULT_GROUP`：預設群組 1–10（預設 9）

     4. 燒錄至手持式 ESP8266 裝置。

     > 所有手燈可燒錄同一份韌體，再透過手機頁面設定各自的群組。

     > `AP_CHANNEL` 必須維持為 1，與 Master / Gateway 相同，否則收不到 ESP-NOW 訊號。

   C. 中繼節點 (Gateway Node) - 選配

    1. 開啟 ```firmware/gateway/gateway.ino```。

    2. 燒錄至用於訊號中繼的 ESP8266。

    3. 部署於場地死角或訊號邊緣處以延伸覆蓋範圍。

## 使用說明 (Usage Guide)

### 中央控制模式 (活動模式)
1. 將主發射節點 (Master) 透過 USB 連接至電腦。

2. 啟動 Node.js 伺服器並開啟網頁介面。

3. 在介面右上方選擇對應的 Serial Port 並點擊「連線」（連線後同一顆按鈕會變成「斷線」）。

4. 介面分成三區：

    - **場景**（左）：已儲存的場景。點一下載入到預覽，Shift + 點擊或 ⋯ 選單中的「直接送上現場」會立即播出。⋯ 選單還可以覆寫、重新命名、排序、刪除。
    - **預覽 / 現場**（右上）：上排是預覽（編輯中，不會送出），下排是現場（手燈正在顯示）。預覽和現場不同的群組右上角會出現黃點。
    - **編輯**（右下）：調整目前選取群組的模式與參數。參數區只顯示目前模式用得到的滑桿。

5. 基本流程：

    1. 點選預覽區的群組或按數字鍵（Ctrl / ⌘ + 點擊或 Shift + 數字可多選，`A` 全選）。
    2. 調整模式、顏色、BPM 等。多選時只會修改你動到的那一項，其他設定各自保留。
    3. 設定淡入時間（0–3 秒），按 **GO**（或 `Enter`）一次送上現場。
    4. 滿意的話按「＋ 將預覽存成場景」，之後點一下即可叫出。

6. PREVIEW / LIVE 切換（右上角，或按 `L`）：

    - **PREVIEW**（預設）：所有調整只影響預覽，按 GO 才送出。
    - **LIVE**：所有調整直接送到手燈，適合即時跟拍。開啟時上方邊框會變紅。

7. 連線時會自動把畫面上的「現場」狀態完整送一次，確保手燈與畫面一致。重新整理頁面時，現場狀態會從伺服器還原。

8. 鍵盤快捷鍵：

    | 按鍵 | 功能 |
    |------|------|
    | `1`–`9`、`0` | 選擇 G1–G9、G10 |
    | `Shift` + 數字 | 加選 / 取消群組 |
    | `A` | 全選群組 |
    | `W` `E` `R` `S` `D` `F` | 恆亮 / 呼吸 / 彩虹 / 窗口 / 節拍 / 漸變 |
    | `X` | 關閉 (OFF) |
    | `Space` | Tap BPM |
    | `Enter` | GO（送出預覽的變更） |
    | `L` | 切換 PREVIEW / LIVE |

    場景沒有快捷鍵：點一下載入到預覽，`Shift` + 點擊直接送上現場。

9. 場景存在專案資料夾的 `scenes.json`，可以直接備份或複製到其他電腦使用。

### 獨立控制模式 (備援模式)
若終端節點超過 5 秒未接收到主控訊號：

1. 裝置將自動切換至獨立模式。

2. 使用手機連接手燈的 Wi-Fi（預設 ```LightStick_001```，或你設定過的名稱）。

3. 控制介面將自動彈出 (Captive Portal)。

4. Android 注意事項: 若介面未彈出，請暫時關閉行動數據，並手動瀏覽 ```http://192.168.4.1```。

5. 手機頁右上角會顯示狀態：
    - 「獨立模式」：手機上的調整會立即生效。
    - 「中控同步中」：正在接收中控訊號，手機上的調整要等中控停止 5 秒後才會生效。

### 修改手燈 Wi-Fi 與群組

1. 連上手燈並打開控制頁面，切換到「設定」分頁。

2. 可修改的項目：

    | 項目 | 規則 |
    |------|------|
    | Wi-Fi 名稱 (SSID) | 1–32 個字元（中文字約佔 3 個字元） |
    | 新密碼 | 8–63 個字元；留空表示維持目前密碼 |
    | 群組 | 1–10 |

3. 若手燈已設定密碼，須輸入「目前密碼」才能儲存。尚未設定密碼時，頁面會顯示提醒，建議盡快設定。

4. 按下「儲存設定」：
    - 只改群組：立即生效，不會斷線。
    - 改了 Wi-Fi 名稱或密碼：約 1 秒後手燈重新啟動 Wi-Fi，手機會斷線，請改連新的 Wi-Fi 名稱。

5. 設定存在 EEPROM 中，關機或重新開機後仍會保留。

### 手燈 HTTP API

| 方法 | 路徑 | 說明 |
|------|------|------|
| GET | `/set?m=&bri=&bpm=&c=&spd=&spr=&dty=&pal=` | 設定獨立模式燈效（與 v1 相同） |
| GET | `/config` | 回傳 `ssid`、`group`、`hasPw`、`online`、`mac`、`fw` 及目前燈效 `st`（不含密碼） |
| POST | `/config` | 表單參數 `ssid`、`npw`（新密碼，可空）、`group`、`cpw`（目前密碼）；回傳 `{"ok":true,"restart":bool}` 或 `{"ok":false,"field":"...","err":"..."}` |

### 修改手機頁介面

手機頁 HTML 內嵌在 `lightstick.ino` 的 `R"rawliteral(` 與 `)rawliteral"` 之間（存放於 Flash / PROGMEM）。建議修改流程：

1. 編輯 `firmware/lightstick/phone_ui_preview.html`，直接用瀏覽器打開預覽（非手燈環境會自動使用假資料）。

2. 確認無誤後，將整份內容貼回 `.ino` 中上述兩個標記之間，再重新燒錄。

## 故障排除 (Troubleshooting)
- 電腦無法連線: 確認 USB 線材具備資料傳輸功能（非僅充電線）。檢查 COM Port 是否被其他軟體佔用（如 Arduino Serial Monitor）。

- LED 顏色錯誤: 檢查 Slave.ino 中的 COLOR_ORDER 設定（常見為 RGB 或 GRB）。

- 訊號延遲: 確保 Master 與 Slave 位於相同的 Wi-Fi Channel（預設為 1）。若場地過大或有遮蔽物，請部署 Gateway 節點。

- 忘記手燈密碼: 手燈沒有硬體重設鍵，需重新燒錄。注意 Arduino IDE 預設的「Erase Flash: Only Sketch」**不會**清除 EEPROM，請擇一處理：
    - 燒錄時將「工具 → Erase Flash」設為「All Flash Contents」；或
    - 將 `lightstick.ino` 中的 `CFG_MAGIC` 改成其他數值後重新燒錄，開機時會自動恢復預設值。

- 手機調整燈光沒有反應: 檢查手機頁右上角是否顯示「中控同步中」。中控台持續廣播時（含 500ms 心跳），手機的設定不會生效，請先在中控台按「斷線」。

- 手燈完全沒反應: 確認 Master、Gateway、手燈都燒錄了同一版本的韌體。只要有一支還是舊版，封包格式就對不上。

- 群組切換仍有先後順序: 確認 Master 已更新為最新韌體，且中控台是透過 GO 送出（舊介面 `/api/send` 沒有批次功能）。

- 改完 Wi-Fi 後找不到手燈: 手機可能仍記住舊的網路設定，請在手機 Wi-Fi 列表中「忘記」舊網路後重新搜尋。

## 授權 (License)
本專案為開源專案，僅供教育與非商業用途使用。
