# ESP-NOW 無線燈光控制系統 (ESC)

## 專案概述 (Project Overview)
ESP-NOW 無線燈光控制系統 (ESC) 是一套專為大型活動、演唱會與互動展演設計的高效能、低延遲分散式燈光控制解決方案。

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
## 核心功能 (Key Features)

- 極低延遲傳輸: 利用 OSI 模型第二層 (Layer 2) 的 ESP-NOW 協定，實現低於 10ms 的響應速度。

- 時間同步機制: 實作主從時間偏差校正演算法，確保所有裝置的動畫相位（如呼吸燈頻率）精確同步。

- 高可靠性: 採用「連發機制 (Burst Fire)」與心跳封包 (Heartbeat)，確保指令在充滿干擾的環境中仍能送達。

- 高擴充性: 支援群組控制（預設 10 個獨立群組 + 全域廣播）。

- 自動故障轉移 (Failover): 當接收不到主控訊號超過 5 秒，裝置自動進入獨立 AP 模式。

- Android 相容性優化: 針對 Android 系統優化 Captive Portal DNS 行為 (類 RFC 8908)，解決控制頁面無法彈出的問題。
    
## 技術規格 (Technical Specifications)

### 硬體需求

- 微控制器: ESP8266 (NodeMCU v2/v3 或 Wemos D1 Mini) x 3+。

- 燈光設備: WS2812B / SK6812 可定址 LED 燈條。

- 電源供應: 5V 直流電源或行動電源。

- 連接介面: Micro-USB 傳輸線 (用於主發射節點)。

### 軟體技術棧

- 後端服務: Node.js, Express.js, SerialPort.

- 前端介面: HTML5, CSS3, Vanilla JavaScript (SPA 架構).

- 韌體開發: Arduino IDE (C++), FastLED Library, ESP8266WiFi.

- 通訊協定資料結構
 
### 系統透過 ESP-NOW 傳輸固定大小的 C Struct：

| 欄位名稱 | 類型 | 說明 |
|--------- | ------- | -------------------- |
| msgId | uint8_t | 訊息序號，用於過濾重複封包 |
| targetGroup | uint8_t | 目標群組 (0=全體, 1-10=特定群組) |
| mode | uint8_t | 運作模式 (1=恆亮, 2=呼吸, 等) |
| bpm | uint16_t | 節奏速率 (Beats Per Minute) |
| color | uint32_t | 主色代碼 (HEX) |
| pal[4] | uint32_t | 4色漸變色盤 |
| timestamp | uint32_t | 主系統時間戳記，用於同步運算 |

## 安裝與部署 (Installation and Deployment)
1. 伺服器端設置 (PC Server)
請確保控制電腦已安裝 Node.js 環境。
```bash
# 進入伺服器目錄
cd server

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
  
     1. 開啟 ```Master/Master.ino```。

     2. 燒錄至連接電腦的 ESP8266。

     3. 注意: 燒錄完成後，需在網頁介面選擇正確的 COM Port 進行連線。

    B. 終端燈具節點 (Slave Node)
  
     1. 開啟 Slave/Slave.ino。

     2. 配置:

        - 修改 MY_GROUP_ID 以設定該裝置所屬群組。

        - 根據硬體連接修改 LED_PIN 與 NUM_LEDS。

    3. 燒錄至手持式 ESP8266 裝置。

   C. 中繼節點 (Gateway Node) - 選配

    1. 開啟 Gateway/Gateway.ino。

    2. 燒錄至用於訊號中繼的 ESP8266。

    3. 部署於場地死角或訊號邊緣處以延伸覆蓋範圍。

## 使用說明 (Usage Guide)

### 中央控制模式 (活動模式)
1. 將主發射節點 (Master) 透過 USB 連接至電腦。

2. 啟動 Node.js 伺服器並開啟網頁介面。

3. 在介面上方選擇對應的 Serial Port 並點擊 「連線」。

4. 使用儀表板控制燈光：

    - 群組控制: 選擇特定群組以創造波浪或分區效果。

    - Tap BPM: 配合現場音樂節奏點擊 "Tap" 按鈕，同步燈光呼吸頻率。

### 獨立控制模式 (備援模式)
若終端節點超過 5 秒未接收到主控訊號：

1. 裝置將自動切換至獨立模式。

2. 使用手機連接 Wi-Fi SSID ```LightStick_XXX```。

3. 控制介面將自動彈出 (Captive Portal)。

4. Android 注意事項: 若介面未彈出，請暫時關閉行動數據，並手動瀏覽 ```http://192.168.4.1```。

## 故障排除 (Troubleshooting)
- 電腦無法連線: 確認 USB 線材具備資料傳輸功能（非僅充電線）。檢查 COM Port 是否被其他軟體佔用（如 Arduino Serial Monitor）。

- LED 顏色錯誤: 檢查 Slave.ino 中的 COLOR_ORDER 設定（常見為 RGB 或 GRB）。

- 訊號延遲: 確保 Master 與 Slave 位於相同的 Wi-Fi Channel（預設為 1）。若場地過大或有遮蔽物，請部署 Gateway 節點。

## 授權 (License)
本專案為開源專案，僅供教育與非商業用途使用。
