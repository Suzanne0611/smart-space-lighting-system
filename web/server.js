const express = require('express');
const fs      = require('fs');
const path    = require('path');
const mqtt    = require('mqtt');

const app  = express();
const PORT = 3000;

const DEV_LIGHTING = '/dev/lighting';

app.use(express.json());
app.use(express.static(path.join(__dirname, 'public')));

// ── MQTT ──────────────────────────────────────────────────────────────────────
const mqttClient = mqtt.connect('mqtt://localhost:1883');

// MQTT state cache 
let cachedLux         = null;
let cachedPir         = null;
let cachedState       = null;
let cachedIdleCounter = 0;
let manualOverride    = false;

mqttClient.on('connect', () => {
    console.log('[server] MQTT connected');
    mqttClient.subscribe('smartspace/lighting/lux',          (err) => { if (err) console.error('[server] 訂閱 lux 失敗:', err.message); });
    mqttClient.subscribe('smartspace/lighting/state',        (err) => { if (err) console.error('[server] 訂閱 state 失敗:', err.message); });
    mqttClient.subscribe('smartspace/lighting/presence',     (err) => { if (err) console.error('[server] 訂閱 presence 失敗:', err.message); });
    mqttClient.subscribe('smartspace/lighting/idle_counter', (err) => { if (err) console.error('[server] 訂閱 idle_counter 失敗:', err.message); });
});

mqttClient.on('error', (err) => {
    console.error('[server] MQTT error:', err.message);
});

mqttClient.on('message', (topic, message) => {
    const val = message.toString();
    if (topic === 'smartspace/lighting/lux')          cachedLux         = parseFloat(val);
    if (topic === 'smartspace/lighting/state')        cachedState       = val;
    if (topic === 'smartspace/lighting/presence')     cachedPir         = parseInt(val);
    if (topic === 'smartspace/lighting/idle_counter') cachedIdleCounter = parseInt(val);
});

function mqttPublish(topic, payload) {
    mqttClient.publish(topic, String(payload), { qos: 0, retain: false });
}

// ── send command to Pico W (via /dev/lighting) ─────────────────────────────────────
function sendCommand(cmd) {
    try {
        const buf = Buffer.from(cmd + '\n');
        const fd  = fs.openSync(DEV_LIGHTING, 'w');
        fs.writeSync(fd, buf);
        fs.closeSync(fd);
        console.log(`[server] TX: ${cmd}`);
        return true;
    } catch (err) {
        console.error(`[server] TX failed: ${err.message}`);
        return false;
    }
}

// ── API ───────────────────────────────────────────────────────────────────────

app.post('/api/mode', (req, res) => {
    const { mode } = req.body;
    if (![0, 1].includes(mode)) return res.status(400).json({ error: '無效模式' });

    manualOverride = (mode !== 0);
    mqttPublish('smartspace/lighting/override', mode);
    console.log(`[server] published override: ${mode} (${manualOverride ? '手動' : '自動'})`);

    if (manualOverride) {
        sendCommand('STATE:ACTIVE');
    }

    res.json({ success: true, manualOverride, command: `MODE:${mode}` });
});

app.post('/api/color', (req, res) => {
    const { color } = req.body;
    if (![0, 1, 2].includes(color)) return res.status(400).json({ error: '無效顏色' });
    const ok = sendCommand(`COLOR:${color}`);
    res.json({ success: ok, command: `COLOR:${color}` });
});

app.post('/api/led', (req, res) => {
    const { r, g, b, brightness } = req.body;
    const cmd = `LED:R${r}G${g}B${b}L${brightness}`;
    const ok  = sendCommand(cmd);
    res.json({ success: ok, command: cmd });
});

app.get('/api/status', (req, res) => {
    res.json({
        lux:            cachedLux,
        pir:            cachedPir,
        state:          cachedState,
        idleCounter:    cachedIdleCounter,
        manualOverride: manualOverride
    });
});

app.listen(PORT, () => {
    console.log(`[server] Server running at http://localhost:${PORT}`);
});
