const hives = new Map();
const gateways = new Map();
const history = new Map();
const holdMs = 10 * 60 * 1000;
const staleMs = 3 * 60 * 1000;
const channel = document.querySelector('#connection');
const body = document.querySelector('#hives');
const gatewayList = document.querySelector('#gateways');
const choice = document.querySelector('#hive');
const canvas = document.querySelector('#history');
const ctx = canvas.getContext('2d');

function draw() {
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    const now = Date.now();
    const start = now - holdMs;
    for (const [id, values] of history) {
        const remaining = values.filter(([time]) => time >= start);
        if (remaining.length) history.set(id, remaining);
        else history.delete(id);
    }
    const points = history.get(choice.value) ?? [];
    ctx.font = '16px system-ui';
    ctx.fillStyle = '#182b29';
    if (!points.length) {
        ctx.fillText('Нет измерений массы за последние 10 минут', 16, 35);
        return;
    }
    const values = points.map(([, kg]) => kg);
    const min = Math.min(...values);
    const max = Math.max(...values);
    const spread = Math.max(max - min, 0.01);
    const left = 85;
    const right = 860;
    const top = 24;
    const bottom = 228;
    ctx.strokeStyle = '#c4d1cd';
    ctx.beginPath();
    ctx.moveTo(left, top);
    ctx.lineTo(left, bottom);
    ctx.lineTo(right, bottom);
    ctx.stroke();
    ctx.fillText(max.toFixed(2) + ' кг', 2, top + 6);
    ctx.fillText(min.toFixed(2) + ' кг', 2, bottom);
    ctx.fillText('−10 мин', left, bottom + 25);
    ctx.fillText('Сейчас', right - 55, bottom + 25);
    ctx.strokeStyle = '#167a5a';
    ctx.fillStyle = '#167a5a';
    ctx.beginPath();
    points.forEach(([time, kg], index) => {
        const x = left + Math.min(1, (time - start) / holdMs) * (right - left);
        const y = max === min ? (top + bottom) / 2 : bottom - (kg - min) / spread * (bottom - top);
        if (index === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
        ctx.fillRect(x - 2, y - 2, 4, 4);
    });
    ctx.stroke();
}

function render() {
    body.replaceChildren();
    gatewayList.replaceChildren();
    const selected = choice.value;
    choice.replaceChildren(new Option('Выбери улей', ''));
    for (const [id, data] of hives) {
        const row = body.insertRow();
        const stale = !data.ts || Date.now() - data.ts > staleMs;
        row.className = stale ? 'stale' : data.swarm_flag ? 'swarm' : '';
        const values = [id, data.node_addr ?? '—',
            Number.isFinite(data.weight_g) ? (data.weight_g / 1000).toFixed(2) : '—',
            Number.isFinite(data.rms) && data.rms >= 0 ? data.rms.toFixed(5) : '—',
            data.band_100_200_ratio ?? '—', data.swarm_flag ? 'Да' : 'Нет',
            Number.isFinite(data.battery_pct) ? data.battery_pct + '%' : '—',
            data.ts ? new Date(data.ts).toLocaleTimeString() + (stale ? ' (устарело)' : '') : 'Время не синхронизировано'];
        for (const value of values) row.insertCell().textContent = value;
        choice.add(new Option(id, id));
    }
    choice.value = hives.has(selected) ? selected : hives.keys().next().value ?? '';
    for (const [id, data] of gateways) {
        const li = document.createElement('li');
        li.textContent = id + ' — ' + data.status;
        gatewayList.append(li);
    }
    draw();
}

const broker = mqtt.connect('ws://' + location.hostname + ':9001/mqtt');
broker.on('connect', () => {
    channel.textContent = 'MQTT подключён';
    broker.subscribe(['apiary/+/hive/+/telemetry', 'apiary/+/gateway/+/status']);
});
broker.on('offline', () => { channel.textContent = 'Связь с брокером потеряна'; });
broker.on('error', () => { channel.textContent = 'Ошибка подключения к MQTT'; });
broker.on('message', (topic, payload) => {
    const [, apiary, kind, name] = topic.split('/');
    const id = apiary + '/' + name;
    const target = kind === 'hive' ? hives : gateways;
    if (payload.length === 0) {
        target.delete(id);
        if (kind === 'hive') history.delete(id);
        render();
        return;
    }
    let data;
    try {
        data = JSON.parse(payload.toString());
    } catch {
        return;
    }
    if (!data || typeof data !== 'object' || Array.isArray(data)) return;
    target.set(id, data);
    if (kind === 'hive' && Number.isFinite(data.ts) && data.ts > 0 && Number.isFinite(data.weight_g)) {
        const points = history.get(id) ?? [];
        if (!points.some(([ts]) => ts === data.ts)) points.push([data.ts, data.weight_g / 1000]);
        points.sort(([first], [second]) => first - second);
        history.set(id, points);
    }
    render();
});
choice.addEventListener('change', draw);
setInterval(render, 10000);
