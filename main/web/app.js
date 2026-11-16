'use strict';

// ---- Protocol v1 (see main/motion_protocol.h; all fields little-endian) ----
const PROTOCOL_VERSION = 1;
const HEADER_SIZE = 16;
const SAMPLE_SIZE = 16;
const FLAG_READ_ERROR = 0x01;
const FLAG_SYNTHETIC = 0x02;
const FLAG_LATE = 0x04;

function decodePacket(buffer) {
  const view = new DataView(buffer);
  if (buffer.byteLength < HEADER_SIZE) throw new Error('packet too short');
  if (view.getUint8(0) !== 0x50 || view.getUint8(1) !== 0x57) throw new Error('bad magic');
  const version = view.getUint8(2);
  if (version !== PROTOCOL_VERSION) throw new Error('unsupported protocol version ' + version);
  const headerSize = view.getUint8(3);
  const sampleSize = view.getUint8(4);
  const count = view.getUint8(5);
  if (headerSize !== HEADER_SIZE || sampleSize !== SAMPLE_SIZE) throw new Error('unexpected header/sample size');
  if (buffer.byteLength !== headerSize + count * sampleSize) throw new Error('length mismatch');
  const packetSeq = view.getUint32(8, true);
  const periodUs = view.getUint32(12, true);
  const samples = new Array(count);
  for (let i = 0, o = headerSize; i < count; i++, o += sampleSize) {
    samples[i] = {
      seq: view.getUint32(o, true),
      ts32: view.getUint32(o + 4, true),
      x: view.getInt16(o + 8, true),
      y: view.getInt16(o + 10, true),
      squal: view.getUint8(o + 12),
      motion: view.getUint8(o + 13),
      flags: view.getUint8(o + 14),
    };
  }
  return { packetSeq, periodUs, samples };
}

// ---- Sequence tracking: unwrap uint32 values and detect gaps ----
const U32 = 4294967296;

function seqDiff(a, b) {
  // Signed distance a - b for uint32 sequence numbers.
  let d = (a - b) % U32;
  if (d > U32 / 2) d -= U32;
  if (d < -U32 / 2) d += U32;
  return d;
}

const tracker = {
  bootId: null,
  nextSeq: null,
  lastTs32: null,
  tsHigh: 0,
  nextPacketSeq: null,
  received: 0,
  lost: 0,
  lostPackets: 0,
  duplicates: 0,
  gaps: [],
  periodUs: 0,
  resetForBoot(bootId) {
    this.bootId = bootId;
    this.nextSeq = null;
    this.lastTs32 = null;
    this.tsHigh = 0;
  },
  unwrapTs(ts32) {
    if (this.lastTs32 !== null && ts32 < this.lastTs32 && this.lastTs32 - ts32 > U32 / 2) this.tsHigh += U32;
    this.lastTs32 = ts32;
    return this.tsHigh + ts32;
  },
  // Returns the samples that are new (duplicates removed), annotated with tsUs.
  accept(packet) {
    if (this.nextPacketSeq !== null && packet.packetSeq !== this.nextPacketSeq) {
      const d = seqDiff(packet.packetSeq, this.nextPacketSeq);
      if (d > 0) this.lostPackets += d;
    }
    this.nextPacketSeq = (packet.packetSeq + 1) % U32;
    this.periodUs = packet.periodUs;
    const fresh = [];
    for (const s of packet.samples) {
      if (this.nextSeq !== null) {
        const d = seqDiff(s.seq, this.nextSeq);
        if (d < 0) { this.duplicates++; continue; }
        if (d > 0) {
          this.lost += d;
          this.gaps.push({ fromSeq: this.nextSeq, count: d, atTsUs: this.lastTs32 === null ? null : this.tsHigh + this.lastTs32 });
          logLine('欠落検出: seq ' + this.nextSeq + ' から ' + d + ' サンプル');
        }
      }
      this.nextSeq = (s.seq + 1) % U32;
      s.tsUs = this.unwrapTs(s.ts32);
      this.received++;
      fresh.push(s);
    }
    return fresh;
  },
};

// ---- Measured sample rate from ESP32 timestamps (last ~1 s) ----
const rateWindow = [];
function updateRate(tsUs) {
  rateWindow.push(tsUs);
  while (rateWindow.length > 2 && tsUs - rateWindow[0] > 1e6) rateWindow.shift();
}
function measuredHz() {
  if (rateWindow.length < 2) return null;
  return (rateWindow.length - 1) / ((rateWindow[rateWindow.length - 1] - rateWindow[0]) / 1e6);
}

// ---- WebSocket link with automatic reconnect and resume ----
const link = {
  ws: null,
  wanted: false,
  state: 'disconnected',
  lastMessageAt: 0,
  reconnectTimer: null,
  connect() {
    this.wanted = true;
    this.open();
  },
  disconnect() {
    this.wanted = false;
    clearTimeout(this.reconnectTimer);
    if (this.ws) this.ws.close();
    this.setState('disconnected');
  },
  open() {
    clearTimeout(this.reconnectTimer);
    if (this.ws) { this.ws.onclose = null; this.ws.close(); }
    this.setState('connecting');
    const ws = new WebSocket('ws://' + location.host + '/ws');
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    ws.onopen = () => { this.lastMessageAt = performance.now(); this.setState('waiting'); };
    ws.onmessage = (event) => { this.lastMessageAt = performance.now(); this.onMessage(event.data); };
    ws.onerror = () => logLine('WebSocket エラー');
    ws.onclose = () => {
      this.ws = null;
      if (this.wanted) {
        this.setState('reconnecting');
        this.reconnectTimer = setTimeout(() => this.open(), 1000);
      } else {
        this.setState('disconnected');
      }
    };
  },
  onMessage(data) {
    if (typeof data === 'string') {
      const message = JSON.parse(data);
      if (message.type === 'hello') this.onHello(message);
      else if (message.type === 'status') onStatus(message);
      return;
    }
    let packet;
    try {
      packet = decodePacket(data);
    } catch (error) {
      logLine('パケット破棄: ' + error.message);
      return;
    }
    for (const s of tracker.accept(packet)) onSample(s);
  },
  onHello(hello) {
    tracker.nextPacketSeq = 0;
    let from = null;
    if (tracker.bootId === hello.boot_id && tracker.nextSeq !== null) {
      from = tracker.nextSeq;
      logLine('再接続: seq ' + from + ' から再開を要求');
    } else {
      if (tracker.bootId !== null && tracker.bootId !== hello.boot_id) logLine('ESP32 の再起動を検出（連番をリセット）');
      tracker.resetForBoot(hello.boot_id);
      logLine('接続: boot_id=' + hello.boot_id + ', 周期 ' + hello.period_us + ' us, ' + hello.packet_samples + ' サンプル/パケット');
    }
    document.getElementById('chipSynthetic').classList.toggle('hidden', !hello.synthetic);
    this.ws.send(JSON.stringify(from === null ? { cmd: 'stream' } : { cmd: 'stream', from }));
    this.setState('open');
  },
  setState(state) {
    this.state = state;
    const labels = {
      disconnected: ['WebSocket: 未接続', ''],
      connecting: ['WebSocket: 接続中…', ''],
      waiting: ['WebSocket: 応答待ち…', ''],
      open: ['WebSocket: 受信中', 'ok'],
      reconnecting: ['WebSocket: 再接続中…', 'bad'],
    };
    const [text, cls] = labels[state];
    const chip = document.getElementById('chipWs');
    chip.textContent = text;
    chip.className = 'chip ' + cls;
    document.getElementById('btnConnect').disabled = this.wanted;
    document.getElementById('btnDisconnect').disabled = !this.wanted;
  },
};

// Close a silent connection so it reconnects and resumes from the ring buffer.
setInterval(() => {
  if (link.ws && link.state === 'open' && performance.now() - link.lastMessageAt > 3000) {
    logLine('3 秒間受信なし: 再接続します');
    link.open();
  }
}, 500);

// ---- Data handling ----
let lastSample = null;
function onSample(s) {
  updateRate(s.tsUs);
  lastSample = s;
}

let lastStatus = null;
function onStatus(status) {
  lastStatus = status;
  const hz = status.window_us > 0 ? (status.window_samples * 1e6 / status.window_us).toFixed(2) : '-';
  const motionPct = status.window_samples > 0 ? (100 * status.motion_samples / status.window_samples).toFixed(0) : '-';
  document.getElementById('status').textContent =
    '取得周波数 ' + hz + ' Hz / 周期 ' + status.min_period_us + '〜' + status.max_period_us + ' us' +
    ' / SPI読出し最大 ' + status.max_read_us + ' us\n' +
    'Motion bit7 ありの割合 ' + motionPct + ' % / 遅延読出し累計 ' + status.late_reads +
    ' / 読出しエラー累計 ' + status.read_errors + '\n' +
    '未送信 ' + status.backlog + ' / リング上書きで欠落 ' + status.ring_lost +
    ' / 送信パケット ' + status.packets + ' / 送信失敗 ' + status.send_failures + '\n' +
    'Wi-Fi 接続台数 ' + status.stations + ' / RSSI ' + status.rssi_dbm + ' dBm' +
    ' / 空きヒープ ' + status.free_heap + ' (最小 ' + status.min_free_heap + ') bytes';
}

// ---- ESP32 reachability over HTTP when the WebSocket is not open ----
async function pollInfo() {
  const chip = document.getElementById('chipEsp');
  if (link.state === 'open') {
    chip.textContent = 'ESP32: 接続中';
    chip.className = 'chip ok';
    return;
  }
  try {
    const response = await fetch('/api/info', { cache: 'no-store' });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    await response.json();
    chip.textContent = 'ESP32: 応答あり';
    chip.className = 'chip ok';
  } catch (error) {
    chip.textContent = 'ESP32: 応答なし';
    chip.className = 'chip bad';
  }
}

// ---- UI ----
function logLine(text) {
  const log = document.getElementById('log');
  const time = new Date().toLocaleTimeString();
  log.textContent = (time + '  ' + text + '\n' + log.textContent).slice(0, 8000);
}

function refresh() {
  document.getElementById('valReceived').textContent = tracker.received;
  document.getElementById('valLost').textContent = tracker.lost;
  document.getElementById('valLostPackets').textContent = tracker.lostPackets;
  const hz = measuredHz();
  document.getElementById('valHz').textContent = hz === null ? '-' : hz.toFixed(1) + ' Hz';
  if (lastSample) {
    const s = lastSample;
    document.getElementById('valLast').textContent =
      'seq=' + s.seq + ' t=' + s.tsUs + ' us  X=' + s.x + '  Y=' + s.y + '  SQUAL=' + s.squal +
      '  motion=0x' + s.motion.toString(16) + '  flags=0x' + s.flags.toString(16);
  }
}

document.getElementById('btnConnect').onclick = () => link.connect();
document.getElementById('btnDisconnect').onclick = () => link.disconnect();
setInterval(refresh, 200);
setInterval(pollInfo, 2000);
pollInfo();
link.setState('disconnected');
