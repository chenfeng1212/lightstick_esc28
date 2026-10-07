// =====================================================================
//  ESC 中控台伺服器 v3
//  - /api/go       一次送出多個群組（預覽 → GO），Master 會讓它們同時切換
//  - /api/scenes   讀寫 scenes.json（場景庫）
//  - /api/program  記住目前現場狀態，重新整理頁面不會遺失
//  - /api/send     v2 舊介面（相容保留）
// =====================================================================
const express = require('express');
const fs = require('fs');
const path = require('path');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const PORT = 3000;
const SCENES_FILE = path.join(__dirname, 'scenes.json');
const NUM_GROUPS = 10;

let port = null;
let program = null;            // 目前現場狀態：長度 10 的陣列（G1–G10）

app.use(express.static(path.join(__dirname, 'public')));
app.use(express.json({ limit: '1mb' }));

// ---------- 工具 ----------
const hex = (v, d = '000000') => (/^[0-9a-fA-F]{6}$/.test(String(v || '')) ? String(v) : d);
const num = (v, d) => (Number.isFinite(+v) ? +v : d);

// 一個群組狀態 → 序列埠的欄位字串
function stateFields(s) {
    const pal = Array.isArray(s.pal || s.p) ? (s.pal || s.p) : [];
    return [
        Math.round(num(s.mode, 0)),
        Math.round(num(s.bri, 200)),
        Math.round(num(s.bpm, 120)),
        hex(s.color || s.col, '00ccff'),
        num(s.speed ?? s.spd, 1.2).toFixed(2),
        num(s.spread ?? s.spr, 0.6).toFixed(2),
        num(s.duty ?? s.dty, 0.5).toFixed(2),
        hex(pal[0]), hex(pal[1]), hex(pal[2]), hex(pal[3]),
    ].join(',');
}

function writeSerial(text) {
    return new Promise((resolve, reject) => {
        if (!port || !port.isOpen) return reject(new Error('尚未連線'));
        port.write(text, err => (err ? reject(err) : port.drain(e => (e ? reject(e) : resolve()))));
    });
}

// ---------- Serial ----------
app.get('/api/ports', async (req, res) => {
    try { res.json(await SerialPort.list()); }
    catch (err) { res.status(500).json({ error: err.message }); }
});

app.post('/api/connect', (req, res) => {
    const { path: p, baudRate } = req.body;
    if (port && port.isOpen) port.close();
    let answered = false;
    const reply = (code, body) => { if (!answered) { answered = true; res.status(code).json(body); } };
    try {
        port = new SerialPort({ path: p, baudRate: parseInt(baudRate) || 115200 });
        const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
        parser.on('data', d => console.log('Master:', d.trim()));
        port.on('open', () => { console.log(`已連線至 ${p}`); reply(200, { success: true }); });
        port.on('error', err => { console.error('Serial 錯誤:', err.message); reply(500, { success: false, error: err.message }); });
    } catch (err) {
        reply(500, { success: false, error: err.message });
    }
});

app.post('/api/disconnect', (req, res) => {
    if (!port || !port.isOpen) return res.json({ success: true, message: '原本就沒連線' });
    port.write('STOP\n', () => {
        setTimeout(() => {
            port.close(err => {
                if (err) return res.status(500).json({ error: err.message });
                port = null;
                console.log('Master 已停止廣播並斷線');
                res.json({ success: true });
            });
        }, 50);
    });
});

// ---------- GO：一次送出多個群組 ----------
// body: { fade: ms, slots: [{ gid, mode, bri, bpm, col, spd, spr, dty, p:[4] }, ...] }
app.post('/api/go', async (req, res) => {
    const { fade = 0, slots = [] } = req.body || {};
    const valid = slots.filter(s => Number.isInteger(+s.gid) && +s.gid >= 0 && +s.gid <= NUM_GROUPS);
    if (!valid.length) return res.status(400).json({ success: false, error: '沒有可送出的群組' });

    const lines = valid.map(s => `S,${+s.gid},${stateFields(s)}`);
    lines.push(`GO,${Math.max(0, Math.min(10000, Math.round(num(fade, 0))))}`);

    try {
        await writeSerial(lines.join('\n') + '\n');
        if (!program) program = Array.from({ length: NUM_GROUPS }, () => null);
        valid.forEach(s => {
            const { gid, ...st } = s;
            if (+gid === 0) program = program.map(() => ({ ...st }));
            else program[+gid - 1] = { ...st };
        });
        res.json({ success: true });
    } catch (err) {
        res.status(err.message === '尚未連線' ? 400 : 500).json({ success: false, error: err.message });
    }
});

app.get('/api/program', (req, res) => {
    res.json({ groups: program });
});

// ---------- 場景 ----------
function readScenes() {
    try { return JSON.parse(fs.readFileSync(SCENES_FILE, 'utf8')); }
    catch (e) { return []; }
}

app.get('/api/scenes', (req, res) => res.json(readScenes()));

app.put('/api/scenes', (req, res) => {
    const scenes = req.body;
    if (!Array.isArray(scenes)) return res.status(400).json({ success: false, error: '格式錯誤' });
    try {
        // 先寫暫存檔再改名，避免寫到一半當機把檔案弄壞
        const tmp = SCENES_FILE + '.tmp';
        fs.writeFileSync(tmp, JSON.stringify(scenes, null, 2), 'utf8');
        fs.renameSync(tmp, SCENES_FILE);
        res.json({ success: true });
    } catch (err) {
        res.status(500).json({ success: false, error: err.message });
    }
});

// ---------- version2 相容：單一群組立即送出 ----------
app.post('/api/send', async (req, res) => {
    const s = req.body || {};
    try {
        await writeSerial(`S,${+s.gid || 0},${stateFields(s)}\nGO,0\n`);
        res.json({ success: true });
    } catch (err) {
        res.status(400).json({ error: err.message });
    }
});

app.listen(PORT, () => {
    console.log(`中控台伺服器已啟動: http://localhost:${PORT}`);
    console.log(`場景檔案: ${SCENES_FILE}`);
});
