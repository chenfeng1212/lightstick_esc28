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

app.post('/api/disconnect', (req, res) => {
    if (port && port.isOpen) {
        port.write("STOP\n", (err) => {
            if (err) console.error("發送 STOP 失敗:", err);
            
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

app.post('/api/send', (req, res) => {
    if (!port || !port.isOpen) return res.status(400).json({ error: "尚未連線" });

    const { gid, mode, bri, bpm, color, speed, spread, duty, pal = [] } = req.body;

    const p1 = pal[0] || "000000";
    const p2 = pal[1] || "000000";
    const p3 = pal[2] || "000000";
    const p4 = pal[3] || "000000";

    const command = `${gid},${mode},${bri},${bpm},${color},${speed},${spread},${duty},${p1},${p2},${p3},${p4}\n`;

    port.write(command, (err) => {
        if (err) return res.status(500).json({ error: err.message });
        // console.log('發送:', command.trim()); 
        res.json({ success: true });
    });
});

app.listen(PORT, () => {
    console.log(`中控台伺服器已啟動: http://localhost:${PORT}`);

});
