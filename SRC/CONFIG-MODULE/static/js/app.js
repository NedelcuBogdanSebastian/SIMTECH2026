'use strict';

const MAX_MODULES = 19;
const TOP_COUNT   = 10;
const BOT_COUNT   = 9;

const TYPE_LABELS = { 0:'Empty', 1:'Dig. CRS', 2:'Digital', 3:'4–20 mA', 4:'PT100' };
const TYPE_IMAGES = {
    0: null,
    1: '/static/images/DwCRS.png',
    2: '/static/images/D.png',
    3: '/static/images/I.png',
    4: '/static/images/T.png',
};

let connected  = false;
let liveMode   = false;
let pollTimer  = null;
let modules    = new Array(MAX_MODULES).fill(0);

const portSelect  = document.getElementById('port-select');
const btnScan     = document.getElementById('btn-scan');
const btnConnect  = document.getElementById('btn-connect');
const btnWrite    = document.getElementById('btn-write');
const btnClear    = document.getElementById('btn-clear');
const btnLive     = document.getElementById('btn-live');
const btnModeRtu  = document.getElementById('mode-rtu');
const btnModeTcp  = document.getElementById('mode-tcp');
const rtuFields   = document.getElementById('rtu-fields');
const tcpFields   = document.getElementById('tcp-fields');
const tcpHost     = document.getElementById('tcp-host');
const tcpPort     = document.getElementById('tcp-port');
const statusPill  = document.getElementById('status-pill');
const statusText  = document.getElementById('status-text');
const activeCount = document.getElementById('active-count');
const deviceCount = document.getElementById('device-count');
const toastEl     = document.getElementById('toast');
const ambientPill = document.getElementById('ambient-pill');
const ambientVal  = document.getElementById('ambient-val');

let connMode = 'rtu';  // 'rtu' or 'tcp'

// ── Mode toggle ───────────────────────────────────────────────────────────────
btnModeRtu.addEventListener('click', () => {
    if (connected) return;
    connMode = 'rtu';
    btnModeRtu.classList.add('active');
    btnModeTcp.classList.remove('active');
    rtuFields.style.display = 'flex';
    tcpFields.style.display = 'none';
});

btnModeTcp.addEventListener('click', () => {
    if (connected) return;
    connMode = 'tcp';
    btnModeTcp.classList.add('active');
    btnModeRtu.classList.remove('active');
    tcpFields.style.display = 'flex';
    rtuFields.style.display = 'none';
});

// ── Build grid ────────────────────────────────────────────────────────────────
function buildGrid() {
    buildSlots('slots-top',    0,         TOP_COUNT);
    buildSlots('slots-bottom', TOP_COUNT, BOT_COUNT);
}

function buildSlots(containerId, startIdx, count) {
    const c = document.getElementById(containerId);
    c.innerHTML = '';
    for (let i = 0; i < count; i++) c.appendChild(createSlot(startIdx + i));
}

function createSlot(idx) {
    const slot = document.createElement('div');
    slot.className = 'slot';
    slot.id = `slot-${idx}`;

    // Slot number
    const num = document.createElement('span');
    num.className = 'slot-num';
    num.textContent = String(idx + 1).padStart(2, '0');

    // ── CONFIG VIEW: image wrap ──
    const imgWrap = document.createElement('div');
    imgWrap.className = 'slot-img-wrap';
    imgWrap.id = `slot-img-${idx}`;
    imgWrap.innerHTML = '<div class="slot-empty-icon">○</div>';
    imgWrap.addEventListener('click', () => {
        if (liveMode) return;
        modules[idx] = (modules[idx] + 1) % 5;
        document.getElementById(`slot-sel-${idx}`).value = modules[idx];
        updateSlotVisual(idx);
        enforceContiguous(idx);
        updateStats();
    });

    // ── LIVE VIEW: data panel ──
    const livePanel = document.createElement('div');
    livePanel.className = 'slot-live';
    livePanel.id = `slot-live-${idx}`;
    // Built on first render

    // Type selector
    const sel = document.createElement('select');
    sel.className = 'slot-select';
    sel.id = `slot-sel-${idx}`;
    sel.title = `Module ${idx + 1}`;
    Object.entries(TYPE_LABELS).forEach(([val, label]) => {
        const opt = document.createElement('option');
        opt.value = val; opt.textContent = label;
        sel.appendChild(opt);
    });
    sel.addEventListener('change', () => {
        modules[idx] = parseInt(sel.value);
        updateSlotVisual(idx);
        enforceContiguous(idx);
        updateStats();
    });

    slot.appendChild(num);
    slot.appendChild(imgWrap);
    slot.appendChild(livePanel);
    slot.appendChild(sel);
    return slot;
}

// ── Config view visual ────────────────────────────────────────────────────────
function updateSlotVisual(idx) {
    const type    = modules[idx];
    const slot    = document.getElementById(`slot-${idx}`);
    const imgWrap = document.getElementById(`slot-img-${idx}`);
    const sel     = document.getElementById(`slot-sel-${idx}`);
    if (!slot) return;

    sel.value = type;

    if (type === 0) {
        slot.classList.remove('filled');
        imgWrap.innerHTML = '<div class="slot-empty-icon">○</div>';
    } else {
        slot.classList.add('filled');
        const src = TYPE_IMAGES[type];
        imgWrap.innerHTML = src
            ? `<img class="slot-img" src="${src}" alt="${TYPE_LABELS[type]}">`
            : `<div class="slot-empty-icon">${type}</div>`;
    }

    // Rebuild live panel structure when type changes
    buildLivePanel(idx, type, null);
}

function refreshAllSlots() {
    for (let i = 0; i < MAX_MODULES; i++) updateSlotVisual(i);
    updateStats();
}

function updateStats() {
    activeCount.textContent = modules.filter(t => t !== 0).length;
}

// ── Enforce contiguous list — no gaps allowed ─────────────────────────────────
// When a slot is set to empty, all slots after it are also cleared.
// When a slot is set to a type, all slots before it must not be empty —
// if they are, this slot cannot be set (show a toast instead).
function enforceContiguous(idx) {
    const type = modules[idx];
    if (type === 0) {
        // Clear all slots after this one
        let changed = false;
        for (let i = idx + 1; i < MAX_MODULES; i++) {
            if (modules[i] !== 0) {
                modules[i] = 0;
                changed = true;
            }
        }
        if (changed) {
            for (let i = idx + 1; i < MAX_MODULES; i++) updateSlotVisual(i);
            showToast('Trailing slots cleared — list must be contiguous', 'info');
        }
    } else {
        // Check if there is an empty slot before this one
        for (let i = 0; i < idx; i++) {
            if (modules[i] === 0) {
                // Revert this slot back to empty
                modules[idx] = 0;
                updateSlotVisual(idx);
                showToast(`Fill slot ${i + 1} first — no gaps allowed`, 'err');
                return;
            }
        }
    }
}

// ── Build live panel DOM (empty/placeholder data) ─────────────────────────────
function buildLivePanel(idx, type, data) {
    const lp = document.getElementById(`slot-live-${idx}`);
    if (!lp) return;
    lp.innerHTML = '';

    if (type === 0) return;

    if (type === 1 || type === 2) {
        const count = type === 1 ? 11 : 12;
        const bits  = data ? data.bits : new Array(count).fill(false);

        const top = document.createElement('div');
        top.className = 'dots-top';
        for (let i = 0; i < 6; i++) {
            const d = document.createElement('div');
            d.className = 'dot' + (bits[i] ? ' on' : '');
            d.id = `dot-${idx}-${i}`;
            top.appendChild(d);
        }

        const div = document.createElement('div');
        div.className = 'live-divider';

        const bot = document.createElement('div');
        bot.className = 'dots-bot';
        for (let i = 6; i < count; i++) {
            const d = document.createElement('div');
            d.className = 'dot' + (bits[i] ? ' on' : '');
            d.id = `dot-${idx}-${i}`;
            bot.appendChild(d);
        }

        lp.appendChild(top);
        lp.appendChild(div);
        lp.appendChild(bot);

        if (type === 1) {
            const crs = document.createElement('div');
            crs.className = 'crs-counter';
            crs.id = `crs-${idx}`;
            crs.textContent = data ? data.counter : '—';
            lp.appendChild(crs);
        }

    } else if (type === 3) {
        // Layout: [v1][v2] / [v3] / divider / [v4][v5] / [v6]
        const values = data ? data.values : Array(6).fill({ma: 0.0});

        function makeChipRow(indices, centerSingle) {
            const row = document.createElement('div');
            row.className = centerSingle ? 'chips-center' : 'chips-top';
            indices.forEach(i => {
                const v   = values[i];
                const pct = Math.min(100, Math.max(0, ((v.ma - 4) / 16) * 100));
                const chip = document.createElement('div');
                chip.className = 'analog-chip';
                chip.id = `chip-${idx}-${i}`;
                chip.innerHTML = `
                    <span class="chip-val color-ma">${v.ma.toFixed(1)}</span>
                    <span class="chip-unit">mA</span>
                    <div class="chip-bar-wrap"><div class="chip-bar" style="width:${pct}%"></div></div>`;
                row.appendChild(chip);
            });
            return row;
        }

        lp.appendChild(makeChipRow([0, 1], false));
        lp.appendChild(makeChipRow([2], true));
        lp.appendChild(Object.assign(document.createElement('div'), {className: 'live-divider'}));
        lp.appendChild(makeChipRow([3, 4], false));
        lp.appendChild(makeChipRow([5], true));

    } else if (type === 4) {
        // 2 top, 2 bottom — integer °C
        const values = data ? data.values : Array(4).fill({temp: 0});

        const topRow = document.createElement('div');
        topRow.className = 'chips-top';
        const div = document.createElement('div');
        div.className = 'live-divider';
        const botRow = document.createElement('div');
        botRow.className = 'chips-bot';

        values.forEach((v, i) => {
            const chip = document.createElement('div');
            chip.className = 'analog-chip';
            chip.id = `chip-${idx}-${i}`;
            chip.innerHTML = `
                <span class="chip-val color-temp">${Math.round(v.temp)}</span>
                <span class="chip-unit">°C</span>`;
            (i < 2 ? topRow : botRow).appendChild(chip);
        });

        lp.appendChild(topRow);
        lp.appendChild(div);
        lp.appendChild(botRow);
    }
}

// ── Update live panel in-place (no DOM rebuild) ───────────────────────────────
function updateLivePanel(idx, data) {
    const type = modules[idx];
    if (type === 0 || !data) return;

    if (type === 1 || type === 2) {
        const count = type === 1 ? 11 : 12;
        const bits  = data.bits || [];
        for (let i = 0; i < count; i++) {
            const d = document.getElementById(`dot-${idx}-${i}`);
            if (d) d.className = 'dot' + (bits[i] ? ' on' : '');
        }
        if (type === 1) {
            const crs = document.getElementById(`crs-${idx}`);
            if (crs) crs.textContent = data.counter ?? '—';
        }
    } else if (type === 3) {
        (data.values || []).forEach((v, i) => {
            const chip = document.getElementById(`chip-${idx}-${i}`);
            if (!chip) return;
            const pct = Math.min(100, Math.max(0, ((v.ma - 4) / 16) * 100));
            chip.querySelector('.chip-val').textContent = v.ma.toFixed(1);
            chip.querySelector('.chip-bar').style.width = pct + '%';
        });
    } else if (type === 4) {
        (data.values || []).forEach((v, i) => {
            const chip = document.getElementById(`chip-${idx}-${i}`);
            if (chip) chip.querySelector('.chip-val').textContent = Math.round(v.temp);
        });
    }
}

// ── Live mode toggle ──────────────────────────────────────────────────────────
btnLive.addEventListener('click', () => {
    if (!connected) return;
    liveMode = !liveMode;
    document.body.classList.toggle('live-mode', liveMode);
    btnLive.classList.toggle('active', liveMode);

    if (liveMode) {
        startPolling();
        ambientPill.classList.add('visible');
        btnWrite.disabled = true;
    } else {
        stopPolling();
        ambientPill.classList.remove('visible');
        btnWrite.disabled = false;
    }
});

// ── Live data polling ─────────────────────────────────────────────────────────
function startPolling() {
    stopPolling();
    pollLiveData();
    pollTimer = setInterval(pollLiveData, 600);
}

function stopPolling() {
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

async function pollLiveData() {
    if (!connected || !liveMode) return;
    try {
        const res = await fetch('/api/live-data', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ modules: [...modules] }),
        });
        if (!res.ok) return;
        const data = await res.json();
        if (data.error) return;

        if (data.ambient !== null && data.ambient !== undefined) {
            ambientVal.textContent = data.ambient;
        }

        (data.modules || []).forEach((mod, idx) => {
            if (mod.type !== 0) updateLivePanel(idx, mod);
        });
    } catch (_) {}
}

// ── Port scanning ─────────────────────────────────────────────────────────────
async function scanPorts() {
    btnScan.textContent = '…';
    try {
        const res  = await fetch('/api/ports');
        const data = await res.json();
        portSelect.innerHTML = '<option value="">Select a port…</option>';
        (data.ports || []).forEach(p => {
            const opt = document.createElement('option');
            opt.value = p.port;
            opt.textContent = p.desc && p.desc !== p.port ? `${p.port} — ${p.desc}` : p.port;
            portSelect.appendChild(opt);
        });
        if (!data.ports || data.ports.length === 0) showToast('No serial ports found', 'info');
    } catch (e) { showToast('Port scan failed', 'err'); }
    btnScan.textContent = '↻';
}

// ── Connect / Disconnect ──────────────────────────────────────────────────────
btnConnect.addEventListener('click', () => connected ? doDisconnect() : doConnect());

async function doConnect() {
    let body = { mode: connMode };
    if (connMode === 'tcp') {
        const host = tcpHost.value.trim();
        const port = parseInt(tcpPort.value) || 502;
        if (!host) { showToast('Please enter an IP address', 'info'); return; }
        body.host = host;
        body.tcp_port = port;
    } else {
        const port = portSelect.value;
        if (!port) { showToast('Please select a serial port first', 'info'); return; }
        body.port = port;
    }

    btnConnect.textContent = '…';
    try {
        const res  = await fetch('/api/connect', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body),
        });
        const data = await res.json();
        if (data.error) { showToast(data.error, 'err'); btnConnect.textContent = 'Connect'; return; }
        setConnectedUI(true);
        if (data.config) applyConfig(data.config);
        const label = connMode === 'tcp'
            ? `TCP ${body.host}:${body.tcp_port}`
            : body.port;
        showToast(`Connected via ${label}`, 'ok');
    } catch (e) {
        showToast('Connection failed: ' + e.message, 'err');
        btnConnect.textContent = 'Connect';
    }
}

async function doDisconnect() {
    if (liveMode) {
        liveMode = false;
        document.body.classList.remove('live-mode');
        btnLive.classList.remove('active');
        stopPolling();
        ambientPill.classList.remove('visible');
    }
    try { await fetch('/api/disconnect', { method: 'POST' }); } catch (_) {}
    setConnectedUI(false);
    showToast('Disconnected', 'info');
}

function setConnectedUI(isOn) {
    connected = isOn;
    btnConnect.textContent = isOn ? 'Disconnect' : 'Connect';
    btnConnect.classList.toggle('connected', isOn);
    statusPill.className   = isOn ? 'status-pill status-on' : 'status-pill status-off';
    statusText.textContent = isOn ? 'Online' : 'Offline';
    btnWrite.disabled      = !isOn;
    btnLive.disabled       = !isOn;
    if (!isOn) deviceCount.textContent = '—';
}

function applyConfig(config) {
    for (let i = 0; i < MAX_MODULES; i++) modules[i] = config[i] || 0;
    refreshAllSlots();
}

// ── Write config ──────────────────────────────────────────────────────────────
btnWrite.addEventListener('click', async () => {
    if (!connected) return;
    btnWrite.disabled    = true;
    btnWrite.textContent = 'Writing…';
    try {
        const res  = await fetch('/api/write-config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ modules: [...modules] }),
        });
        const data = await res.json();
        if (data.error) showToast(data.error, 'err');
        else {
            deviceCount.textContent = data.moduleCount ?? '?';
            showToast(`Configuration saved — ${data.moduleCount} module(s) active`, 'ok');
        }
    } catch (e) { showToast('Write failed: ' + e.message, 'err'); }
    btnWrite.disabled    = false;
    btnWrite.textContent = 'Write configuration';
});

// ── Clear all ─────────────────────────────────────────────────────────────────
btnClear.addEventListener('click', () => {
    modules.fill(0);
    refreshAllSlots();
    showToast('All slots cleared', 'info');
});

// ── Toast ─────────────────────────────────────────────────────────────────────
let _toastTimer = null;
function showToast(msg, type = 'info') {
    toastEl.textContent = msg;
    toastEl.className   = `toast toast-${type}`;
    if (_toastTimer) clearTimeout(_toastTimer);
    _toastTimer = setTimeout(() => { toastEl.className = 'toast toast-hidden'; }, 4500);
}

// ── Init ──────────────────────────────────────────────────────────────────────
buildGrid();
refreshAllSlots();
scanPorts();
