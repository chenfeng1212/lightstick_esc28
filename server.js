const express = require('express');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');
const app = express();
const PORT = 3000;

let port = null;

app.use(express.static('public'));
app.use(express.json());

app.get('/api/ports', async (req, res) => {
    try {
        const ports = await SerialPort.list();
        res.json(ports);
    } catch (err) {
        res.status(500).json({ error: err.message });
    }
});

app.post('/api/connect', (req, res) => {
    const { path, baudRate } = req.body;
    if (port && port.isOpen) port.close();

    try {
        port = new SerialPort({ path: path, baudRate: parseInt(baudRate) || 115200 });
        const parser = port.pipe(new ReadlineParser({ delimiter: '\r\n' }));
        parser.on('data', (data) => console.log('Master 回傳:', data));

        port.on('open', () => {
            console.log(`已連線至 ${path}`);
            res.json({ success: true });
        });
        port.on('error', (err) => res.status(500).json({ success: false, error: err.message }));
    } catch (err) {
        res.status(500).json({ success: false, error: err.message });
    }
});

// API: 斷開連線
app.post('/api/disconnect', (req, res) => {
    if (port && port.isOpen) {
        // 1. 先發送 STOP 指令
        port.write("STOP\n", (err) => {
            if (err) console.error("發送 STOP 失敗:", err);
            
            // 2. 給 Master 一點時間處理 (例如 50ms)，然後再關閉 Port
            setTimeout(() => {
                port.close((closeErr) => {
                    if (closeErr) {
                        console.error('斷線失敗:', closeErr);
                        return res.status(500).json({ error: closeErr.message });
                    }
                    port = null;
                    console.log('Master 已停止廣播並斷線');
                    res.json({ success: true });
                });
            }, 50);
        });
    } else {
        res.json({ success: true, message: "原本就沒連線" });
    }
});

// API: 發送指令 (更新版：加入色盤)
app.post('/api/send', (req, res) => {
    if (!port || !port.isOpen) return res.status(400).json({ error: "尚未連線" });

    // 解構取得 pal 陣列 (預設為空陣列以防出錯)
    const { gid, mode, bri, bpm, color, speed, spread, duty, pal = [] } = req.body;

    // 確保色盤有 4 個顏色，不足的用 000000 補齊
    const p1 = pal[0] || "000000";
    const p2 = pal[1] || "000000";
    const p3 = pal[2] || "000000";
    const p4 = pal[3] || "000000";

    // 組合新指令字串 (後面多了 4 個顏色)
    // 格式: Gid,Mode,Bri,BPM,Color,Speed,Spread,Duty,P1,P2,P3,P4
    const command = `${gid},${mode},${bri},${bpm},${color},${speed},${spread},${duty},${p1},${p2},${p3},${p4}\n`;

    port.write(command, (err) => {
        if (err) return res.status(500).json({ error: err.message });
        // console.log('發送:', command.trim()); // Debug用
        res.json({ success: true });
    });
});

app.listen(PORT, () => {
    console.log(`中控台伺服器已啟動: http://localhost:${PORT}`);
});