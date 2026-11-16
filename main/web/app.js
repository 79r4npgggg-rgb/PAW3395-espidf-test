'use strict';

// ===================================================================
// Protocol v1 (see main/motion_protocol.h; all fields little-endian)
// ===================================================================
const PROTOCOL_VERSION = 1;
const HEADER_SIZE = 16;
const SAMPLE_SIZE = 16;
const FLAG_READ_ERROR = 0x01;
const FLAG_SYNTHETIC = 0x02;
const FLAG_LATE = 0x04;
const U32 = 4294967296;

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
      packetSeq,
    };
  }
  return { packetSeq, periodUs, samples };
}

// Signed distance a - b between uint32 sequence numbers.
function seqDiff(a, b) {
  let d = (a - b) % U32;
  if (d > U32 / 2) d -= U32;
  if (d < -U32 / 2) d += U32;
  return d;
}

// ===================================================================
// Formatting
// ===================================================================
function fmtPhys(v) {
  if (v === null || v === undefined || !Number.isFinite(v)) return '-';
  const a = Math.abs(v);
  if (a === 0) return '0';
  if (a >= 1e6 || a < 1e-4) return v.toExponential(3);
  if (a >= 1000) return v.toFixed(1);
  if (a >= 100) return v.toFixed(2);
  if (a >= 1) return v.toFixed(3);
  return v.toPrecision(4);
}
function fmtFixed(v, digits) {
  return v === null || v === undefined || !Number.isFinite(v) ? '-' : v.toFixed(digits);
}
// Integers (raw counts, nice grid steps) without decimals; anything else as fmtPhys.
function fmtAuto(v) {
  return Number.isInteger(v) ? String(v) : fmtPhys(v);
}
function fmtDuration(seconds) {
  if (!Number.isFinite(seconds) || seconds < 0) return '-';
  const s = Math.floor(seconds);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const pad = (n) => String(n).padStart(2, '0');
  return (h > 0 ? h + ':' + pad(m) : m) + ':' + pad(s % 60);
}
const $ = (id) => document.getElementById(id);

// ===================================================================
// Physical-unit scaling (kept in this browser's localStorage)
// ===================================================================
const SCALING_KEY = 'paw3395.monitor.scaling.v1';
const scaling = { xSlope: 1, xIntercept: 0, ySlope: 1, yIntercept: 0, unit: 'counts', version: 0 };

function loadScaling() {
  try {
    const saved = JSON.parse(localStorage.getItem(SCALING_KEY) || 'null');
    if (saved) {
      for (const key of ['xSlope', 'xIntercept', 'ySlope', 'yIntercept']) {
        if (Number.isFinite(saved[key])) scaling[key] = saved[key];
      }
      if (typeof saved.unit === 'string') scaling.unit = saved.unit;
    }
  } catch (error) {
    logLine('変換設定を読み込めません: ' + error.message);
  }
}
function saveScaling() {
  const { xSlope, xIntercept, ySlope, yIntercept, unit } = scaling;
  localStorage.setItem(SCALING_KEY, JSON.stringify({ xSlope, xIntercept, ySlope, yIntercept, unit }));
}
const toPhysX = (raw) => raw * scaling.xSlope + scaling.xIntercept;
const toPhysY = (raw) => raw * scaling.ySlope + scaling.yIntercept;

// Unit of the time integral of the physical value (for the trajectory).
function integratedUnit() {
  const unit = scaling.unit.trim();
  if (/\/s$/.test(unit)) return unit.slice(0, -2);
  return unit ? unit + '·s' : '·s';
}

// ===================================================================
// Sequence tracking: duplicates, gaps, timestamp unwrapping
// ===================================================================
const tracker = {
  bootId: null,
  nextSeq: null,
  lastTs32: null,
  tsHigh: 0,
  nextPacketSeq: null,
  periodUs: 0,
  received: 0,
  packets: 0,
  lost: 0,
  lostPackets: 0,
  duplicates: 0,
  resetCounters() {
    this.received = 0;
    this.packets = 0;
    this.lost = 0;
    this.lostPackets = 0;
    this.duplicates = 0;
  },
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
  // Returns only new samples, each annotated with tsUs (unwrapped) and gapBefore.
  accept(packet) {
    this.packets++;
    if (this.nextPacketSeq !== null && packet.packetSeq !== this.nextPacketSeq) {
      const d = seqDiff(packet.packetSeq, this.nextPacketSeq);
      if (d > 0) {
        this.lostPackets += d;
        recorder.onPacketLoss(d);
      }
    }
    this.nextPacketSeq = (packet.packetSeq + 1) % U32;
    this.periodUs = packet.periodUs;
    const fresh = [];
    for (const s of packet.samples) {
      s.gapBefore = 0;
      if (this.nextSeq !== null) {
        const d = seqDiff(s.seq, this.nextSeq);
        if (d < 0) { this.duplicates++; continue; }
        if (d > 0) {
          this.lost += d;
          s.gapBefore = d;
          onGap(this.nextSeq, d);
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

// ===================================================================
// Display ring buffer (newest samples for the time-series charts)
// ===================================================================
class SampleRing {
  constructor(capacity) {
    this.cap = capacity;
    this.t = new Float64Array(capacity);
    this.x = new Int16Array(capacity);
    this.y = new Int16Array(capacity);
    this.q = new Uint8Array(capacity);
    this.gap = new Uint8Array(capacity);
    this.clear();
  }
  clear() { this.start = 0; this.length = 0; }
  push(s) {
    let i;
    if (this.length < this.cap) {
      i = (this.start + this.length) % this.cap;
      this.length++;
    } else {
      i = this.start;
      this.start = (this.start + 1) % this.cap;
    }
    this.t[i] = s.tsUs;
    this.x[i] = s.x;
    this.y[i] = s.y;
    this.q[i] = s.squal;
    this.gap[i] = s.gapBefore > 0 ? 1 : 0;
  }
  idx(k) { return (this.start + k) % this.cap; }
  // First logical index whose timestamp is >= tMin (timestamps are increasing).
  lowerBound(tMin) {
    let lo = 0;
    let hi = this.length;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.t[this.idx(mid)] < tMin) lo = mid + 1; else hi = mid;
    }
    return lo;
  }
}
const ring = new SampleRing(32768);

// ===================================================================
// Trajectory: cumulative displacement in the sensor frame
// ===================================================================
// Raw: sum of Delta counts.  Physical: integral of (raw*slope + intercept) dt,
// stored as sum(raw*dt) and sum(dt) so any slope/intercept can be applied later.
const TRAJ_MAX_POINTS = 200000;
const traj = {
  t: new Float64Array(TRAJ_MAX_POINTS),
  sx: new Float64Array(TRAJ_MAX_POINTS),
  sy: new Float64Array(TRAJ_MAX_POINTS),
  sxdt: new Float64Array(TRAJ_MAX_POINTS),
  sydt: new Float64Array(TRAJ_MAX_POINTS),
  sdt: new Float64Array(TRAJ_MAX_POINTS),
  count: 0,
  stride: 1,
  counter: 0,
  cur: { t: 0, sx: 0, sy: 0, sxdt: 0, sydt: 0, sdt: 0 },
  lastTsUs: null,
  reset() {
    this.count = 0;
    this.stride = 1;
    this.counter = 0;
    this.cur = { t: 0, sx: 0, sy: 0, sxdt: 0, sydt: 0, sdt: 0 };
    this.lastTsUs = null;
  },
  store(p) {
    if (this.count === TRAJ_MAX_POINTS) {
      // Keep every other point and halve the storage rate from now on.
      let j = 0;
      for (let i = 0; i < this.count; i += 2, j++) {
        this.t[j] = this.t[i]; this.sx[j] = this.sx[i]; this.sy[j] = this.sy[i];
        this.sxdt[j] = this.sxdt[i]; this.sydt[j] = this.sydt[i]; this.sdt[j] = this.sdt[i];
      }
      this.count = j;
      this.stride *= 2;
    }
    const i = this.count++;
    this.t[i] = p.t; this.sx[i] = p.sx; this.sy[i] = p.sy;
    this.sxdt[i] = p.sxdt; this.sydt[i] = p.sydt; this.sdt[i] = p.sdt;
  },
  add(s) {
    // Delta covers the time since the previous read.  If that read was lost its
    // interval is unknown, so use the scheduled period.
    const nominal = (tracker.periodUs || 10000) / 1e6;
    const dt = this.lastTsUs !== null && !s.gapBefore ? (s.tsUs - this.lastTsUs) / 1e6 : nominal;
    this.lastTsUs = s.tsUs;
    const c = this.cur;
    c.t = s.tsUs;
    c.sx += s.x;
    c.sy += s.y;
    c.sxdt += s.x * dt;
    c.sydt += s.y * dt;
    c.sdt += dt;
    if (this.count === 0 || ++this.counter >= this.stride) {
      this.counter = 0;
      this.store(c);
    }
  },
};
function trajPoint(mode, sx, sy, sxdt, sydt, sdt) {
  if (mode === 'raw') return [sx, sy];
  return [scaling.xSlope * sxdt + scaling.xIntercept * sdt, scaling.ySlope * sydt + scaling.yIntercept * sdt];
}

// ===================================================================
// Statistics since the last reset
// ===================================================================
class Running {
  constructor() { this.reset(); }
  reset() { this.n = 0; this.mean = 0; this.m2 = 0; this.min = Infinity; this.max = -Infinity; }
  add(v) {
    this.n++;
    const d = v - this.mean;
    this.mean += d / this.n;
    this.m2 += d * (v - this.mean);
    if (v < this.min) this.min = v;
    if (v > this.max) this.max = v;
  }
  get std() { return this.n > 1 ? Math.sqrt(this.m2 / (this.n - 1)) : 0; }
}
const stats = {
  x: new Running(),
  y: new Running(),
  q: new Running(),
  magRaw: new Running(),
  magPhys: new Running(),
  magPhysVersion: 0,
  firstTs: null,
  lastTs: null,
  lost: 0,
  since: null,
  reset() {
    for (const r of [this.x, this.y, this.q, this.magRaw, this.magPhys]) r.reset();
    this.magPhysVersion = scaling.version;
    this.firstTs = null;
    this.lastTs = null;
    this.lost = 0;
    this.since = new Date();
  },
  add(s) {
    if (s.flags & FLAG_READ_ERROR) return;
    this.x.add(s.x);
    this.y.add(s.y);
    this.q.add(s.squal);
    this.magRaw.add(Math.hypot(s.x, s.y));
    if (this.magPhysVersion !== scaling.version) {
      this.magPhys.reset();
      this.magPhysVersion = scaling.version;
    }
    this.magPhys.add(Math.hypot(toPhysX(s.x), toPhysY(s.y)));
    if (this.firstTs === null) { this.firstTs = s.tsUs; this.firstSeq = s.seq; }
    this.lastTs = s.tsUs;
    this.lastSeq = s.seq;
  },
  // Reads per second performed by the ESP32 during the statistics period.
  acquisitionHz() {
    const span = this.firstTs === null ? 0 : (this.lastTs - this.firstTs) / 1e6;
    return span > 0 ? seqDiff(this.lastSeq, this.firstSeq) / span : null;
  },
};

// ===================================================================
// Rates
// ===================================================================
const sampleTimes = []; // {ts, seq} of the last ~1 s of samples (ESP32 clock)
const packetTimes = []; // PC arrival times of the last ~2 s of packets
// Acquisition rate: reads performed by the ESP32 per second, from sequence
// numbers and ESP32 timestamps (unaffected by samples lost on the way).
function measuredHz() {
  if (sampleTimes.length < 2) return null;
  const first = sampleTimes[0];
  const last = sampleTimes[sampleTimes.length - 1];
  return seqDiff(last.seq, first.seq) / ((last.ts - first.ts) / 1e6);
}
// Received samples per second of ESP32 time (drops when samples are lost).
function receivedHz() {
  if (sampleTimes.length < 2) return null;
  return (sampleTimes.length - 1) / ((sampleTimes[sampleTimes.length - 1].ts - sampleTimes[0].ts) / 1e6);
}
function packetRate() {
  if (packetTimes.length < 2) return null;
  return (packetTimes.length - 1) / ((packetTimes[packetTimes.length - 1] - packetTimes[0]) / 1000);
}

// ===================================================================
// Recorder: every received sample from START to STOP, kept apart from the
// display buffers, in chunked typed arrays (about 23 bytes per sample)
// ===================================================================
const RECORD_CHUNK = 65536;
const RECORD_MAX_SAMPLES = 2000000; // about 5.5 h at 100 Hz, about 46 MB
const RECORD_BYTES_PER_SAMPLE = 23;
const MAX_GAPS_IN_CSV = 1000;

function newRecordChunk() {
  return {
    seq: new Uint32Array(RECORD_CHUNK),
    ts: new Float64Array(RECORD_CHUNK),
    x: new Int16Array(RECORD_CHUNK),
    y: new Int16Array(RECORD_CHUNK),
    q: new Uint8Array(RECORD_CHUNK),
    m: new Uint8Array(RECORD_CHUNK),
    f: new Uint8Array(RECORD_CHUNK),
    p: new Uint32Array(RECORD_CHUNK),
  };
}

function isoLocal(date) {
  const pad = (n, w = 2) => String(n).padStart(w, '0');
  const off = -date.getTimezoneOffset();
  return date.getFullYear() + '-' + pad(date.getMonth() + 1) + '-' + pad(date.getDate()) + 'T' +
    pad(date.getHours()) + ':' + pad(date.getMinutes()) + ':' + pad(date.getSeconds()) + '.' +
    pad(date.getMilliseconds(), 3) + (off >= 0 ? '+' : '-') + pad(Math.floor(Math.abs(off) / 60)) + ':' + pad(Math.abs(off) % 60);
}
function stampForFile(date) {
  const pad = (n) => String(n).padStart(2, '0');
  return date.getFullYear() + pad(date.getMonth() + 1) + pad(date.getDate()) + '_' +
    pad(date.getHours()) + pad(date.getMinutes()) + pad(date.getSeconds());
}
// Shortest decimal text that round-trips to 12 significant digits.
const csvNumber = (v) => String(Number(v.toPrecision(12)));

const recorder = {
  state: 'idle', // idle | recording | stopped
  chunks: [],
  count: 0,
  exported: true,
  startedAt: null,
  stoppedAt: null,
  stopReason: '',
  bootId: null,
  periodUs: 0,
  firmware: '',
  lost: 0,
  lostPackets: 0,
  gaps: [],
  start() {
    this.chunks = [];
    this.count = 0;
    this.exported = false;
    this.startedAt = new Date();
    this.stoppedAt = null;
    this.stopReason = '';
    this.bootId = tracker.bootId;
    this.periodUs = tracker.periodUs;
    this.firmware = deviceInfo ? deviceInfo.firmware : '';
    this.lost = 0;
    this.lostPackets = 0;
    this.gaps = [];
    this.state = 'recording';
  },
  stop(reason) {
    if (this.state !== 'recording') return;
    this.state = 'stopped';
    this.stoppedAt = new Date();
    this.stopReason = reason;
  },
  push(s) {
    if (this.state !== 'recording') return;
    if (this.count >= RECORD_MAX_SAMPLES) {
      this.stop('max_samples');
      logLine('記録上限 ' + RECORD_MAX_SAMPLES.toLocaleString() + ' サンプルに達したため記録を停止しました');
      updateButtons();
      return;
    }
    const ci = Math.floor(this.count / RECORD_CHUNK);
    if (ci === this.chunks.length) this.chunks.push(newRecordChunk());
    const c = this.chunks[ci];
    const i = this.count % RECORD_CHUNK;
    c.seq[i] = s.seq;
    c.ts[i] = s.tsUs;
    c.x[i] = s.x;
    c.y[i] = s.y;
    c.q[i] = s.squal;
    c.m[i] = s.motion;
    c.f[i] = s.flags;
    c.p[i] = s.packetSeq;
    this.count++;
  },
  onGap(fromSeq, count) {
    if (this.state !== 'recording' || this.count === 0) return;
    this.lost += count;
    this.gaps.push({ fromSeq, count });
  },
  onPacketLoss(count) {
    if (this.state === 'recording' && this.count > 0) this.lostPackets += count;
  },
  at(k) {
    return [this.chunks[Math.floor(k / RECORD_CHUNK)], k % RECORD_CHUNK];
  },
  summary() {
    if (this.count === 0) return null;
    const [c0, i0] = this.at(0);
    const [c1, i1] = this.at(this.count - 1);
    const duration = (c1.ts[i1] - c0.ts[i0]) / 1e6;
    const reads = seqDiff(c1.seq[i1], c0.seq[i0]);
    return {
      firstTs: c0.ts[i0],
      firstSeq: c0.seq[i0],
      lastSeq: c1.seq[i1],
      duration,
      acquisitionHz: duration > 0 ? reads / duration : null,
      receivedHz: duration > 0 ? (this.count - 1) / duration : null,
    };
  },
  buildCsv() {
    const sum = this.summary();
    if (!sum) return null;
    const meta = [
      ['PAW3395 Wireless Monitor CSV'],
      ['csv_format_version', 1],
      ['exported_at', isoLocal(new Date())],
      ['record_start_pc', isoLocal(this.startedAt)],
      ['record_stop_pc', this.stoppedAt ? isoLocal(this.stoppedAt) : ''],
      ['stop_reason', this.stopReason],
      ['firmware', this.firmware],
      ['protocol_version', PROTOCOL_VERSION],
      ['device_boot_id', this.bootId],
      ['nominal_period_us', this.periodUs],
      ['samples', this.count],
      ['first_sequence', sum.firstSeq],
      ['last_sequence', sum.lastSeq],
      ['lost_samples', this.lost],
      ['lost_packets', this.lostPackets],
      ['duration_s', sum.duration.toFixed(6)],
      ['acquisition_rate_hz', sum.acquisitionHz === null ? '' : sum.acquisitionHz.toFixed(3)],
      ['received_rate_hz', sum.receivedHz === null ? '' : sum.receivedHz.toFixed(3)],
      ['x_slope', scaling.xSlope],
      ['x_intercept', scaling.xIntercept],
      ['y_slope', scaling.ySlope],
      ['y_intercept', scaling.yIntercept],
      ['unit', scaling.unit],
      ['note', 'timestamp_us = ESP32 esp_timer microseconds since boot (monotonic; not wall-clock time)'],
      ['note', 'x_raw/y_raw = PAW3395 Delta_X/Delta_Y int16 counts accumulated since the previous read (BLE 0003/0004; BLE 0006/0007 = raw + 32768)'],
      ['note', 'x_processed = x_raw * x_slope + x_intercept; y_processed = y_raw * y_slope + y_intercept [unit]'],
      ['note', 'flags bit0 = SPI read error; bit1 = synthetic test data; bit2 = read started late'],
      ['note', 'missing sequence numbers were not received; each is listed below as gap,<first missing sequence>,<count>'],
    ];
    const parts = [meta.map((row) => '# ' + row.join(',')).join('\n') + '\n'];
    const gapLines = this.gaps.slice(0, MAX_GAPS_IN_CSV).map((g) => '# gap,' + g.fromSeq + ',' + g.count);
    if (this.gaps.length > MAX_GAPS_IN_CSV) gapLines.push('# gap_list_truncated,' + this.gaps.length);
    if (gapLines.length) parts.push(gapLines.join('\n') + '\n');
    parts.push('sequence,timestamp_us,elapsed_ms,squal,x_raw,y_raw,x_processed,y_processed,packet_sequence,motion,flags\n');

    const { xSlope, xIntercept, ySlope, yIntercept } = scaling;
    let rows = [];
    for (let k = 0; k < this.count; k++) {
      const [c, i] = this.at(k);
      const x = c.x[i];
      const y = c.y[i];
      rows.push(c.seq[i] + ',' + c.ts[i] + ',' + ((c.ts[i] - sum.firstTs) / 1000).toFixed(3) + ',' + c.q[i] + ',' +
        x + ',' + y + ',' + csvNumber(x * xSlope + xIntercept) + ',' + csvNumber(y * ySlope + yIntercept) + ',' +
        c.p[i] + ',' + c.m[i] + ',' + c.f[i]);
      if (rows.length === 20000) {
        parts.push(rows.join('\n') + '\n');
        rows = [];
      }
    }
    if (rows.length) parts.push(rows.join('\n') + '\n');

    const hz = sum.acquisitionHz === null ? 'NA' : sum.acquisitionHz.toFixed(2);
    const name = 'paw3395_' + stampForFile(this.startedAt) + '_N' + this.count + '_' + hz + 'Hz.csv';
    return { name, parts };
  },
  exportCsv() {
    const csv = this.buildCsv();
    if (!csv) return;
    const { name, parts } = csv;
    const url = URL.createObjectURL(new Blob(parts, { type: 'text/csv' }));
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    this.exported = true;
    logLine('CSV 出力: ' + name + '（' + this.count.toLocaleString() + ' 行）');
  },
};

// ===================================================================
// Incoming data
// ===================================================================
let lastSample = null;
let lastSampleAt = 0;
let lastStatus = null;
let deviceInfo = null;

function onGap(fromSeq, count) {
  stats.lost += count;
  recorder.onGap(fromSeq, count);
  logLine('欠落検出: seq ' + fromSeq + ' から ' + count + ' サンプル');
}

function onPacket(packet) {
  const now = performance.now();
  packetTimes.push(now);
  while (packetTimes.length > 2 && now - packetTimes[0] > 2000) packetTimes.shift();
  for (const s of tracker.accept(packet)) onSample(s);
}

function onSample(s) {
  sampleTimes.push({ ts: s.tsUs, seq: s.seq });
  while (sampleTimes.length > 2 && s.tsUs - sampleTimes[0].ts > 1e6) sampleTimes.shift();
  ring.push(s);
  traj.add(s);
  stats.add(s);
  recorder.push(s);
  lastSample = s;
  lastSampleAt = performance.now();
}

function onHello(hello) {
  deviceInfo = hello;
  if (tracker.bootId !== null && tracker.bootId !== hello.boot_id) {
    if (recorder.state === 'recording') {
      recorder.stop('device_reboot');
      logLine('ESP32 の再起動を検出したため記録を停止しました（記録済みデータは CSV 出力できます）');
    }
    logLine('ESP32 の再起動を検出: 連番と時刻がリセットされたため表示をクリアします');
    clearDisplay();
  }
  $('chipSynthetic').classList.toggle('hidden', !hello.synthetic);
}

function onStatus(status) {
  lastStatus = status;
  const hz = status.window_us > 0 ? status.window_samples * 1e6 / status.window_us : null;
  $('dRate').textContent = (hz === null ? '-' : hz.toFixed(2) + ' Hz') + ' / ' +
    status.min_period_us + '〜' + status.max_period_us + ' us';
  $('dRead').textContent = status.max_read_us + ' us';
  $('dMotion').textContent = status.window_samples > 0
    ? (100 * status.motion_samples / status.window_samples).toFixed(0) + ' %' : '-';
  $('dLate').textContent = status.late_reads + ' / ' + status.read_errors;
  $('dBacklog').textContent = status.backlog + ' / ' + status.ring_lost;
  $('dSent').textContent = status.packets + ' / ' + status.send_failures;
  $('dHeap').textContent = status.free_heap + ' (' + status.min_free_heap + ') bytes';
  $('mRssi').textContent = status.stations > 0 ? status.rssi_dbm + ' dBm' : '-';
  $('mStations').textContent = 'ESP32 側 RSSI / 接続 ' + status.stations + ' 台';
}

// ===================================================================
// WebSocket link: automatic reconnect and resume from the ESP32 ring buffer
// ===================================================================
const STALL_TIMEOUT_MS = 3000;
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
    if (recorder.state === 'recording') {
      if (!confirm('記録中です。切断すると記録を停止します。切断しますか？')) return;
      recorder.stop('user_disconnect');
      logLine('切断のため記録を停止しました');
    }
    this.wanted = false;
    clearTimeout(this.reconnectTimer);
    if (this.ws) { this.ws.onclose = null; this.ws.close(); this.ws = null; }
    this.setState('disconnected');
    logLine('切断しました');
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
    ws.onerror = () => {};
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
      let message;
      try { message = JSON.parse(data); } catch (error) { logLine('不正な JSON: ' + data.slice(0, 80)); return; }
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
    onPacket(packet);
  },
  onHello(hello) {
    onHello(hello);
    tracker.nextPacketSeq = 0;
    let from = null;
    if (tracker.bootId === hello.boot_id && tracker.nextSeq !== null) {
      from = tracker.nextSeq;
      logLine('再接続: seq ' + from + ' から再開を要求（ESP32 の保持範囲内なら欠落なし）');
    } else {
      tracker.resetForBoot(hello.boot_id);
      logLine('接続: boot_id=' + hello.boot_id + '、周期 ' + hello.period_us + ' us、' +
        hello.packet_samples + ' サンプル/パケット、ESP32 保持 ' + hello.ring_samples + ' サンプル');
    }
    this.ws.send(JSON.stringify(from === null ? { cmd: 'stream' } : { cmd: 'stream', from }));
    this.setState('open');
  },
  setState(state) {
    this.state = state;
    const labels = {
      disconnected: ['WebSocket: 未接続', ''],
      connecting: ['WebSocket: 接続中…', 'warn'],
      waiting: ['WebSocket: 応答待ち…', 'warn'],
      open: ['WebSocket: 受信中', 'ok'],
      reconnecting: ['WebSocket: 再接続中…', 'bad'],
    };
    const [text, cls] = labels[state];
    $('chipWs').textContent = text;
    $('chipWs').className = 'chip ' + cls;
    updateButtons();
  },
};

// A silent connection is closed so that it reconnects and resumes.
setInterval(() => {
  if (link.ws && link.state === 'open' && performance.now() - link.lastMessageAt > STALL_TIMEOUT_MS) {
    logLine((STALL_TIMEOUT_MS / 1000) + ' 秒間受信なし: 再接続します');
    link.open();
  }
}, 500);

// ESP32 reachability over HTTP while the WebSocket is not receiving.
async function pollInfo() {
  const chip = $('chipEsp');
  if (link.state === 'open') {
    chip.textContent = 'ESP32: 接続中';
    chip.className = 'chip ok';
    return;
  }
  try {
    const response = await fetch('/api/info', { cache: 'no-store' });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const info = await response.json();
    chip.textContent = 'ESP32: 応答あり (' + info.ssid + ')';
    chip.className = 'chip ok';
  } catch (error) {
    chip.textContent = 'ESP32: 応答なし';
    chip.className = 'chip bad';
  }
}

// ===================================================================
// Charts
// ===================================================================
function niceStep(range, targetCount) {
  const raw = range / Math.max(1, targetCount);
  const p = Math.pow(10, Math.floor(Math.log10(raw)));
  const m = raw / p;
  return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}
function fmtTick(v, step) {
  const decimals = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  return Math.abs(v) < step / 1e6 ? '0' : v.toFixed(Math.min(decimals, 6));
}
function fitCanvas(canvas) {
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth;
  const h = canvas.clientHeight;
  if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w, h };
}

class TimeSeriesChart {
  constructor(canvas, color) {
    this.canvas = canvas;
    this.color = color;
  }
  draw({ title, unit, value, windowS, zeroLine, format }) {
    const { ctx, w, h } = fitCanvas(this.canvas);
    const L = 64, R = 12, T = 24, B = 22;
    const pw = w - L - R;
    const ph = h - T - B;
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = '#10141a';
    ctx.fillRect(L, T, pw, ph);

    const windowUs = windowS * 1e6;
    const tEnd = ring.length ? ring.t[ring.idx(ring.length - 1)] : 0;
    const tStart = tEnd - windowUs;
    const k0 = ring.length ? ring.lowerBound(tStart) : 0;
    const n = ring.length - k0;

    let lo = Infinity;
    let hi = -Infinity;
    for (let k = k0; k < ring.length; k++) {
      const v = value(ring.idx(k));
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    if (!Number.isFinite(lo)) { lo = -1; hi = 1; }
    if (zeroLine) { lo = Math.min(lo, 0); hi = Math.max(hi, 0); }
    if (hi - lo < 1e-12) { const pad = Math.abs(hi) * 0.1 || 1; lo -= pad; hi += pad; }
    const span = hi - lo;
    lo -= span * 0.08;
    hi += span * 0.08;
    const yOf = (v) => T + ph - (v - lo) / (hi - lo) * ph;
    const xOf = (t) => L + (t - tStart) / windowUs * pw;

    ctx.font = '11px "Yu Gothic UI", "Segoe UI", sans-serif';
    ctx.lineWidth = 1;
    const ys = niceStep(hi - lo, 5);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    for (let v = Math.ceil(lo / ys) * ys; v <= hi; v += ys) {
      const y = Math.round(yOf(v)) + 0.5;
      ctx.strokeStyle = '#262c35';
      ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + pw, y); ctx.stroke();
      ctx.fillStyle = '#8b949e';
      ctx.fillText(fmtTick(v, ys), L - 6, y);
    }
    const ts = windowS <= 1 ? 0.2 : windowS <= 5 ? 1 : windowS <= 10 ? 2 : 5;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    for (let s = 0; s <= windowS + 1e-9; s += ts) {
      const x = Math.round(L + pw - s / windowS * pw) + 0.5;
      ctx.strokeStyle = '#262c35';
      ctx.beginPath(); ctx.moveTo(x, T); ctx.lineTo(x, T + ph); ctx.stroke();
      ctx.fillStyle = '#8b949e';
      ctx.fillText(s === 0 ? '0 s' : '-' + (+s.toFixed(1)) + ' s', x, T + ph + 4);
    }
    if (zeroLine && lo < 0 && hi > 0) {
      const y = Math.round(yOf(0)) + 0.5;
      ctx.strokeStyle = '#6e7681';
      ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + pw, y); ctx.stroke();
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(L, T, pw, ph);
    ctx.clip();
    ctx.strokeStyle = this.color;
    ctx.lineWidth = 1.4;
    ctx.beginPath();
    // Above ~2 points per pixel, draw the min/max of each bucket so peaks stay visible.
    const bucket = Math.max(1, Math.floor(n / (pw * 2)));
    let penDown = false;
    for (let k = k0; k < ring.length; k += bucket) {
      const end = Math.min(ring.length, k + bucket);
      let bmin = Infinity, bmax = -Infinity, gap = false;
      for (let j = k; j < end; j++) {
        const i = ring.idx(j);
        const v = value(i);
        if (v < bmin) bmin = v;
        if (v > bmax) bmax = v;
        if (ring.gap[i]) gap = true;
      }
      const x = xOf(ring.t[ring.idx(k)]);
      if (gap || !penDown) { ctx.moveTo(x, yOf(bmin)); penDown = true; } else { ctx.lineTo(x, yOf(bmin)); }
      if (bmax !== bmin) ctx.lineTo(x, yOf(bmax));
    }
    ctx.stroke();
    ctx.restore();

    ctx.fillStyle = '#e6e6e6';
    ctx.font = '12px "Yu Gothic UI", "Segoe UI", sans-serif';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText(title + (unit ? '  [' + unit + ']' : ''), L, 4);
    if (ring.length) {
      ctx.textAlign = 'right';
      ctx.fillStyle = this.color;
      ctx.fillText('最新 ' + format(value(ring.idx(ring.length - 1))), L + pw, 4);
    }
  }
}

class TrajectoryView {
  constructor(canvas) {
    this.canvas = canvas;
    this.fit = true;
    this.scale = 1; // pixels per unit
  }
  zoom(factor) {
    this.fit = false;
    this.scale *= factor;
  }
  draw({ mode, hold, windowS, unit }) {
    const { ctx, w, h } = fitCanvas(this.canvas);
    ctx.clearRect(0, 0, w, h);
    const c = traj.cur;
    const current = trajPoint(mode, c.sx, c.sy, c.sxdt, c.sydt, c.sdt);
    let i0 = 0;
    if (!hold && traj.count) {
      const tMin = c.t - windowS * 1e6;
      let lo = 0, hi = traj.count;
      while (lo < hi) { const mid = (lo + hi) >> 1; if (traj.t[mid] < tMin) lo = mid + 1; else hi = mid; }
      i0 = lo;
    }
    const stride = Math.max(1, Math.floor((traj.count - i0) / 20000));
    const pts = [];
    for (let i = i0; i < traj.count; i += stride) {
      pts.push(trajPoint(mode, traj.sx[i], traj.sy[i], traj.sxdt[i], traj.sydt[i], traj.sdt[i]));
    }
    pts.push(current);

    // Screen: +X (front/back) points up, +Y (left/right) points right.
    let cx = current[0], cy = current[1];
    if (this.fit) {
      let minX = 0, maxX = 0, minY = 0, maxY = 0; // always include the origin
      for (const [x, y] of pts) {
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
      }
      const span = Math.max(maxX - minX, maxY - minY, 1e-9);
      this.scale = Math.min(w, h) * 0.85 / span;
      cx = (minX + maxX) / 2;
      cy = (minY + maxY) / 2;
    }
    const s = this.scale;
    const sx = (y) => w / 2 + (y - cy) * s;
    const sy = (x) => h / 2 - (x - cx) * s;

    const step = niceStep(Math.min(w, h) / s, 6);
    ctx.lineWidth = 1;
    ctx.strokeStyle = '#222831';
    for (let v = Math.floor((cy - w / 2 / s) / step) * step; v <= cy + w / 2 / s; v += step) {
      const x = Math.round(sx(v)) + 0.5;
      ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    }
    for (let v = Math.floor((cx - h / 2 / s) / step) * step; v <= cx + h / 2 / s; v += step) {
      const y = Math.round(sy(v)) + 0.5;
      ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }
    const ox = sx(0), oy = sy(0);
    ctx.strokeStyle = '#6e7681';
    ctx.beginPath(); ctx.moveTo(ox - 10, oy); ctx.lineTo(ox + 10, oy); ctx.moveTo(ox, oy - 10); ctx.lineTo(ox, oy + 10); ctx.stroke();

    ctx.strokeStyle = '#58a6ff';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    pts.forEach(([x, y], k) => (k ? ctx.lineTo(sx(y), sy(x)) : ctx.moveTo(sx(y), sy(x))));
    ctx.stroke();
    ctx.fillStyle = '#3fb950';
    ctx.beginPath(); ctx.arc(sx(pts[0][1]), sy(pts[0][0]), 3.5, 0, Math.PI * 2); ctx.fill();
    ctx.fillStyle = '#ff7b72';
    ctx.beginPath(); ctx.arc(sx(current[1]), sy(current[0]), 5, 0, Math.PI * 2); ctx.fill();

    ctx.font = '11px "Yu Gothic UI", "Segoe UI", sans-serif';
    ctx.fillStyle = '#8b949e';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'top';
    ctx.fillText('↑ +X 前後   → +Y 左右   原点 ＋', 6, 6);
    ctx.textBaseline = 'bottom';
    ctx.fillText('グリッド ' + fmtAuto(step) + ' ' + unit + (this.fit ? '（自動）' : ''), 6, h - 6);
    ctx.textAlign = 'right';
    ctx.fillText('X ' + fmtAuto(current[0]) + '  Y ' + fmtAuto(current[1]) + ' ' + unit, w - 6, h - 6);
  }
}

// ===================================================================
// UI
// ===================================================================
const charts = {
  x: new TimeSeriesChart($('cvX'), '#58a6ff'),
  y: new TimeSeriesChart($('cvY'), '#d2a8ff'),
  q: new TimeSeriesChart($('cvSqual'), '#3fb950'),
};
const trajView = new TrajectoryView($('cvXY'));
const ui = { mode: 'raw', windowS: 5 };

function logLine(text) {
  const log = $('log');
  const time = new Date().toLocaleTimeString();
  log.textContent = (time + '  ' + text + '\n' + log.textContent).split('\n').slice(0, 300).join('\n');
}

function clearDisplay() {
  ring.clear();
  traj.reset();
  stats.reset();
  sampleTimes.length = 0;
  lastSample = null;
}

function unitLabels() {
  return ui.mode === 'raw'
    ? { xy: 'counts/サンプル', traj: 'counts' }
    : { xy: scaling.unit || '-', traj: integratedUnit() };
}

function drawCharts() {
  const units = unitLabels();
  const raw = ui.mode === 'raw';
  const intFormat = (v) => String(v);
  charts.x.draw({
    title: 'X（前後） 0006 ' + (raw ? '生データ' : '物理量'), unit: units.xy, windowS: ui.windowS, zeroLine: true,
    value: raw ? (i) => ring.x[i] : (i) => toPhysX(ring.x[i]), format: raw ? intFormat : fmtPhys,
  });
  charts.y.draw({
    title: 'Y（左右） 0007 ' + (raw ? '生データ' : '物理量'), unit: units.xy, windowS: ui.windowS, zeroLine: true,
    value: raw ? (i) => ring.y[i] : (i) => toPhysY(ring.y[i]), format: raw ? intFormat : fmtPhys,
  });
  charts.q.draw({
    title: 'SQUAL 0005', unit: '', windowS: ui.windowS, zeroLine: false, value: (i) => ring.q[i], format: intFormat,
  });
}

function drawTrajectory() {
  trajView.draw({ mode: ui.mode, hold: $('chkHold').checked, windowS: ui.windowS, unit: unitLabels().traj });
}

function statCells(prefix, r, slope, intercept) {
  const phys = slope !== null;
  const tf = (v) => (phys ? v * slope + intercept : v);
  let min = r.min, max = r.max;
  if (phys && slope < 0) [min, max] = [max, min];
  const f = phys ? fmtPhys : (v) => fmtFixed(v, 0);
  $(prefix + 'max').textContent = r.n ? f(tf(max)) : '-';
  $(prefix + 'min').textContent = r.n ? f(tf(min)) : '-';
  $(prefix + 'mean').textContent = r.n ? (phys ? fmtPhys(tf(r.mean)) : fmtFixed(r.mean, 2)) : '-';
  $(prefix + 'std').textContent = r.n ? (phys ? fmtPhys(r.std * Math.abs(slope)) : fmtFixed(r.std, 2)) : '-';
}

function updateDom() {
  const hz = measuredHz();
  const rxHz = receivedHz();
  const nominal = tracker.periodUs ? 1e6 / tracker.periodUs : null;
  $('mHz').textContent = hz === null ? '-' : hz.toFixed(1) + ' Hz';
  $('mHz').parentElement.classList.toggle('alert', hz !== null && nominal !== null && hz < nominal * 0.95);
  $('mHzNote').textContent = (nominal === null ? '' : '目標 ' + nominal.toFixed(1) + ' Hz / ') +
    '受信 ' + (rxHz === null ? '-' : rxHz.toFixed(1)) + ' Hz';
  $('mReceived').textContent = tracker.received.toLocaleString();
  const pr = packetRate();
  $('mPackets').textContent = 'パケット ' + tracker.packets.toLocaleString() + (pr === null ? '' : '（' + pr.toFixed(1) + '/s）');
  $('mLost').textContent = tracker.lost.toLocaleString();
  $('mLost').parentElement.classList.toggle('alert', tracker.lost > 0);
  $('mLostPackets').textContent = '欠落パケット ' + tracker.lostPackets + ' / 重複 ' + tracker.duplicates;

  const units = unitLabels();
  $('mXUnit').textContent = units.xy;
  $('mYUnit').textContent = units.xy;
  const s = lastSample;
  if (s) {
    $('mSqual').textContent = s.squal;
    $('mX').textContent = ui.mode === 'raw' ? s.x : fmtPhys(toPhysX(s.x));
    $('mY').textContent = ui.mode === 'raw' ? s.y : fmtPhys(toPhysY(s.y));
  } else {
    $('mSqual').textContent = $('mX').textContent = $('mY').textContent = '-';
  }

  const chip = $('chipSensor');
  if (!s || performance.now() - lastSampleAt > 2000) {
    chip.textContent = 'センサ: データなし';
    chip.className = 'chip';
  } else if (s.flags & FLAG_READ_ERROR) {
    chip.textContent = 'センサ: 読出しエラー';
    chip.className = 'chip bad';
  } else if (s.flags & FLAG_SYNTHETIC) {
    chip.textContent = 'センサ: テストデータ';
    chip.className = 'chip warn';
  } else {
    chip.textContent = 'センサ: 稼働中';
    chip.className = 'chip ok';
  }

  const phys = ui.mode === 'phys';
  statCells('sX', stats.x, phys ? scaling.xSlope : null, scaling.xIntercept);
  statCells('sY', stats.y, phys ? scaling.ySlope : null, scaling.yIntercept);
  statCells('sS', stats.q, null, 0);
  const mag = phys ? stats.magPhys : stats.magRaw;
  $('sMag').textContent = mag.n
    ? '平均 ' + (phys ? fmtPhys(mag.mean) : fmtFixed(mag.mean, 2)) + ' / 最大 ' + (phys ? fmtPhys(mag.max) : fmtFixed(mag.max, 0)) + ' ' + units.xy
    : '-';
  $('sCount').textContent = stats.x.n.toLocaleString();
  const span = stats.firstTs !== null ? (stats.lastTs - stats.firstTs) / 1e6 : 0;
  const statsHz = stats.acquisitionHz();
  $('sHz').textContent = statsHz === null ? '-' : statsHz.toFixed(2) + ' Hz（' + fmtDuration(span) + '）';
  $('sLost').textContent = stats.lost.toLocaleString();
  $('statsSince').textContent = stats.since ? stats.since.toLocaleTimeString() + ' から' : '';
  updateRecordingMetric();
  updateButtons();
  $('xyNote').textContent = ui.mode === 'raw'
    ? 'センサ座標系の累積変位（Delta の総和、counts）。車体のヨー角は補正していないため、旋回すると実際の走行軌跡とは異なります。軸の向き・符号は未確認の仮定です。'
    : 'センサ座標系での物理量の時間積分 ∫(X, Y) dt。単位が「…/s」なら「…」の移動量です。ヨー角は補正していません。';
}

let lastChartDraw = 0;
let lastTrajDraw = 0;
let lastDom = 0;
function frame(now) {
  requestAnimationFrame(frame);
  if (now - lastChartDraw >= 33) { lastChartDraw = now; drawCharts(); }
  if (now - lastTrajDraw >= 66) { lastTrajDraw = now; drawTrajectory(); }
  if (now - lastDom >= 200) { lastDom = now; updateDom(); }
}

function bindScalingInputs() {
  const fields = [
    ['inXSlope', 'xSlope'], ['inXIntercept', 'xIntercept'], ['inYSlope', 'ySlope'], ['inYIntercept', 'yIntercept'],
  ];
  for (const [id, key] of fields) {
    const input = $(id);
    input.value = String(scaling[key]);
    input.addEventListener('input', () => {
      const text = input.value.trim();
      const v = Number(text);
      const ok = text !== '' && Number.isFinite(v);
      input.classList.toggle('invalid', !ok);
      if (!ok || v === scaling[key]) return;
      scaling[key] = v;
      scaling.version++;
      saveScaling();
    });
  }
  const unit = $('inUnit');
  unit.value = scaling.unit;
  unit.addEventListener('input', () => { scaling.unit = unit.value; saveScaling(); });
}

function updateButtons() {
  const recording = recorder.state === 'recording';
  $('btnConnect').disabled = link.wanted;
  $('btnDisconnect').disabled = !link.wanted;
  $('btnStart').disabled = recording || link.state !== 'open';
  $('btnStop').disabled = !recording;
  $('btnExport').disabled = recorder.state !== 'stopped' || recorder.count === 0;
  $('chipRec').classList.toggle('hidden', !recording);
}

function updateRecordingMetric() {
  const sum = recorder.summary();
  const time = $('mRecTime');
  const info = $('mRecInfo');
  if (recorder.state === 'idle') {
    time.textContent = '-';
    info.textContent = '未記録';
    return;
  }
  time.textContent = sum ? fmtDuration(sum.duration) : '0:00';
  const mb = (recorder.count * RECORD_BYTES_PER_SAMPLE / 1048576).toFixed(1);
  const state = recorder.state === 'recording' ? '記録中' : '停止' + (recorder.exported ? '（CSV 出力済）' : '（未出力）');
  info.textContent = state + ' ' + recorder.count.toLocaleString() + ' 件 / ' + mb + ' MB / 欠落 ' + recorder.lost;
  time.parentElement.classList.toggle('alert', recorder.lost > 0);
}

$('btnConnect').onclick = () => link.connect();
$('btnDisconnect').onclick = () => link.disconnect();
$('btnStart').onclick = () => {
  if (recorder.state === 'stopped' && !recorder.exported &&
      !confirm('前回の記録（' + recorder.count.toLocaleString() + ' 件）はまだ CSV 出力されていません。破棄して新しく記録しますか？')) {
    return;
  }
  stats.reset();
  traj.reset();
  recorder.start();
  updateButtons();
  logLine('START: 記録開始（統計と軌跡をリセット）');
};
$('btnStop').onclick = () => {
  recorder.stop('user');
  updateButtons();
  const sum = recorder.summary();
  logLine('STOP: ' + recorder.count.toLocaleString() + ' サンプル、' + (sum ? fmtDuration(sum.duration) : '0:00') +
    '、欠落 ' + recorder.lost + ' サンプル');
};
$('btnExport').onclick = () => {
  recorder.exportCsv();
  updateButtons();
};
$('btnReset').onclick = () => {
  clearDisplay();
  tracker.resetCounters();
  logLine('RESET: グラフ・軌跡・統計・欠落カウンタをクリアしました（記録データは保持）');
};
window.addEventListener('beforeunload', (event) => {
  if (recorder.state === 'recording' || (recorder.state === 'stopped' && !recorder.exported)) {
    event.preventDefault();
    event.returnValue = '';
  }
});
$('selMode').onchange = (e) => { ui.mode = e.target.value; };
$('selWindow').onchange = (e) => { ui.windowS = Number(e.target.value); };
$('btnZoomIn').onclick = () => trajView.zoom(1.5);
$('btnZoomOut').onclick = () => trajView.zoom(1 / 1.5);
$('btnFit').onclick = () => { trajView.fit = true; };
$('btnTrajReset').onclick = () => { traj.reset(); logLine('軌跡をリセットしました'); };

loadScaling();
bindScalingInputs();
stats.reset();
link.setState('disconnected');
pollInfo();
setInterval(pollInfo, 2000);
requestAnimationFrame(frame);
