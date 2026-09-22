// Diagnostic log for the device page: one bounded, subscribable history of what
// was sent, what came back and what the board reported.
//
// Design rules (they are the reason this module exists):
//   * The logger only observes. It never sends anything, never retries, never
//     changes a response and never drives the UI: a broken logger (denied
//     storage, a throwing subscriber, a corrupt payload) cannot break a request.
//   * Everything that enters is allowlist-filtered. Request parameters come from
//     a per-path list, response objects keep only named primitives, and keys that
//     look like credentials are dropped at every depth, so a password can never
//     reach the store, an export or browser storage.
//   * Every collection is bounded (events, CAN frames, string lengths).
//
// Pure helpers are exported separately so they can be unit tested without a
// browser; the store itself takes all its inputs (clock, storage, timers)
// injected.
//
// Honest limits, also shown in the UI: the board log is a RAM ring that is lost
// on power reset and is not the serial console; the browser history belongs to
// this origin only; only observed CAN frames are recorded, and gaps are recorded
// as gaps; and nothing restored here is ever used to drive control.

export const LOG_VERSION = 1;
export const LOG_STORAGE_KEY = 'babytech.device-log.v1';
export const MAX_EVENTS = 500;
export const MAX_TRACE_FRAMES = 400;
export const MAX_TEXT = 160;
export const SAVE_DEBOUNCE_MS = 400;
export const LEVELS = ['debug', 'info', 'warn', 'error'];
export const SOURCES = ['board', 'http', 'can', 'status', 'queue', 'config', 'limits', 'session'];

const SENSITIVE = /pass|secret|token|credential|ssid|auth/i;
const EVENT_DETAIL_MAX = 240;

// ---------------------------------------------------------------------------
// Sanitising
// ---------------------------------------------------------------------------

const primitive = (value) => typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean';
const clipped = (value) => (typeof value === 'string' ? value.slice(0, MAX_TEXT) : value);

/**
 * Request parameters are taken from a fixed allowlist per path - never
 * enumerated - so an arbitrary argument (a queue program, a password, an
 * unknown future field) cannot reach the log. Values stay primitive and short.
 */
export const POST_INPUT_ALLOWLIST = {
  '/api/command': ['hex'],
  '/api/move': ['id', 'angle', 'speed', 'accel', 'decel', 'current'],
  // `enabled` is the key the board actually reads (main.cpp handleEnable).
  '/api/enable': ['id', 'enabled'],
  '/api/stop': ['id'],
  '/api/stop-all': [],
  '/api/control/reset': [],
  // The queue program is the one input whose CONTENT has to survive into the
  // log: "which instruction did I send" is the question this page exists to
  // answer. It is not a credential and it is the user's own text.
  '/api/queue/start': ['repeat', 'program'],
  '/api/queue/cancel': [],
  '/api/limits': ['maxSpeedRpm', 'maxAccelRpmS', 'maxCurrentMa', 'maxAngleDeg', 'maxMoveSeconds', 'experimentSeconds'],
  '/api/scale/tare': [],
  '/api/scale/calibrate': ['knownWeightG'],
  '/api/scale/config': ['doutPin', 'sckPin'],
  '/api/motor-distance': ['id', 'rotationDistance'],
};

// The submitted program is kept as text, not as a one-line parameter: 8192
// characters, with an explicit flag when even that was not enough.
export const PROGRAM_TEXT_MAX = 8192;

export function sanitizeParams(path, params) {
  const allowed = POST_INPUT_ALLOWLIST[path];
  if (!allowed || !params || typeof params !== 'object') return {};
  const out = {};
  for (const key of allowed) {
    if (SENSITIVE.test(key)) continue;
    const value = params[key];
    if (key === 'program' && typeof value === 'string') {
      out.program = value.slice(0, PROGRAM_TEXT_MAX);
      out.programTruncated = value.length > PROGRAM_TEXT_MAX;
      continue;
    }
    if (!primitive(value)) continue;
    out[key] = clipped(value);
  }
  return out;
}

/** "12 行 / 348 字符" - the detail line stays readable, the text is kept whole. */
export function programSummary(text) {
  const lines = String(text ?? '').split('\n').length;
  return `${lines} 行 / ${String(text ?? '').length} 字符`;
}

/**
 * A response body is summarised, never stored: only the fields the board's own
 * error/result shapes carry, as primitives. Unknown or nested values are dropped
 * so no arbitrary payload (or credential) can be captured.
 */
export const RESPONSE_KEYS = ['ok', 'message', 'error', 'line', 'state', 'runId', 'id', 'enabled', 'step', 'total'];

export function summarizeBody(body) {
  if (!body || typeof body !== 'object' || Array.isArray(body)) return {};
  const out = {};
  for (const key of RESPONSE_KEYS) {
    if (SENSITIVE.test(key)) continue;
    const value = body[key];
    if (!primitive(value)) continue;
    out[key] = clipped(value);
  }
  return out;
}

/**
 * Keeps only allowlisted primitive fields of one object, dropping sensitive keys
 * at every level. Used for anything that is copied into an export.
 */
export function redactObject(value, allowed = null, depth = 0) {
  if (!value || typeof value !== 'object' || Array.isArray(value) || depth > 2) return null;
  const keys = allowed ?? Object.keys(value);
  const out = {};
  let count = 0;
  for (const key of keys) {
    if (typeof key !== 'string' || SENSITIVE.test(key)) continue;
    const entry = value[key];
    if (primitive(entry)) out[key] = clipped(entry);
    else if (entry && typeof entry === 'object') {
      const nested = redactObject(entry, null, depth + 1);
      if (nested) out[key] = nested;
    }
    if (++count >= 24) break;
  }
  return Object.keys(out).length ? out : null;
}

export function formatParams(params) {
  const keys = Object.keys(params ?? {});
  if (!keys.length) return '';
  return keys.map((key) => `${key}=${params[key]}`).join(' ');
}

// ---------------------------------------------------------------------------
// Board log payload
// ---------------------------------------------------------------------------

/**
 * Validates GET /api/logs. Anything malformed is rejected as a whole: a partial
 * board ring is not a fact this page may present as history.
 */
export function parseBoardLog(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) return { ok: false, error: 'not_object' };
  const bootId = typeof payload.bootId === 'string' ? payload.bootId.slice(0, 32) : '';
  if (!bootId) return { ok: false, error: 'missing_boot_id' };
  if (!Number.isFinite(Number(payload.sequence))) return { ok: false, error: 'missing_sequence' };
  if (!Array.isArray(payload.events)) return { ok: false, error: 'missing_events' };
  const events = [];
  for (const raw of payload.events) {
    if (!raw || typeof raw !== 'object') continue;
    const seq = Number(raw.seq);
    if (!Number.isFinite(seq)) continue;
    events.push({
      seq,
      atMs: Number.isFinite(Number(raw.atMs)) ? Number(raw.atMs) : 0,
      level: LEVELS.includes(raw.level) ? raw.level : 'info',
      event: typeof raw.event === 'string' ? raw.event.slice(0, 40) : 'event',
      detail: typeof raw.detail === 'string' ? raw.detail.slice(0, EVENT_DETAIL_MAX) : '',
      truncated: raw.truncated === true,
    });
  }
  events.sort((a, b) => a.seq - b.seq);
  return {
    ok: true,
    bootId,
    sequence: Number(payload.sequence),
    capacity: Number.isFinite(Number(payload.capacity)) ? Number(payload.capacity) : events.length,
    // The board reports how many ring entries a bounded response left out. That
    // is NOT a ring overwrite: the events still exist, they were just not sent.
    omitted: Number.isFinite(Number(payload.omitted)) && Number(payload.omitted) > 0
      ? Number(payload.omitted) : 0,
    events,
  };
}

/**
 * What a new board payload means for the cursor: a new boot, a gap the ring can
 * no longer cover, or nothing new. Pure so the three cases stay testable.
 */
export function boardDelta(payload, cursor) {
  const previous = cursor?.bootId ?? '';
  const previousSeq = Number(cursor?.seq ?? 0);
  const restarted = payload.bootId !== previous;
  const oldest = payload.events.length ? payload.events[0].seq : payload.sequence;
  const fresh = restarted ? payload.events : payload.events.filter((event) => event.seq > previousSeq);
  // A ring that no longer holds the next expected sequence means the board
  // overwrote events this browser never saw. It is reported as a gap, never
  // filled in with guesses.
  // A bounded board response ("omitted") explains the same jump without any ring
  // loss, so it must not be reported as a gap the board cannot fill.
  const bounded = Number(payload.omitted ?? 0) > 0;
  const gap = !restarted && !bounded && previousSeq > 0 && oldest > previousSeq + 1
    ? { from: previousSeq + 1, to: oldest - 1 }
    : null;
  return { restarted, fresh, gap, bounded, omitted: Number(payload.omitted ?? 0), sequence: payload.sequence, bootId: payload.bootId };
}

// ---------------------------------------------------------------------------
// Change detection for the polled endpoints
// ---------------------------------------------------------------------------

// Only fields that describe a *state* are compared. Values that change on every
// poll (position, speed, current, ages) are deliberately not part of the key, so
// a running motor does not flood the log.
const STATUS_KEYS = ['state', 'fault', 'enabled', 'canReady', 'lastAck', 'online', 'activeId'];
const QUEUE_KEYS = ['state', 'runId', 'step', 'message', 'line'];
// /api/config-result carries one persistent transaction: its identity, state and
// driver answer. The expected/actual byte arrays are deliberately not compared.
const CONFIG_KEYS = ['state', 'sequence', 'id', 'ack'];

export function pickFields(source, keys) {
  const out = {};
  if (!source || typeof source !== 'object') return out;
  for (const key of keys) {
    const value = source[key];
    if (primitive(value)) out[key] = clipped(value);
  }
  return out;
}

export function diffFields(previous, next) {
  const changed = {};
  for (const key of Object.keys(next ?? {})) {
    if (previous?.[key] !== next[key]) changed[key] = next[key];
  }
  return changed;
}

/**
 * State key for /api/status. `control.blockers` is reduced to "id:reason" pairs:
 * a blocker appearing or disappearing is an event, while its age ticking up on
 * every poll is not.
 */
export function statusKey(status) {
  const key = pickFields(status, STATUS_KEYS);
  const control = status?.control;
  if (control && typeof control === 'object' && !Array.isArray(control)) {
    if (typeof control.busy === 'boolean') key.controlBusy = control.busy;
    const blockers = Array.isArray(control.blockers) ? control.blockers : [];
    key.blockers = blockers
      .map((entry) => `${entry?.id ?? '?'}:${entry?.reason ?? '?'}`)
      .join(',');
    if (Number.isFinite(Number(control.blockerCount))) key.blockerCount = Number(control.blockerCount);
  }
  return key;
}
export function queueKey(queue) { return pickFields(queue, QUEUE_KEYS); }
export function configKey(config) { return pickFields(config, CONFIG_KEYS); }
export function limitsKey(limits) {
  return pickFields(limits, ['maxSpeedRpm', 'maxAccelRpmS', 'maxCurrentMa', 'maxAngleDeg', 'maxMoveSeconds', 'experimentSeconds']);
}

export function formatChanges(changes) {
  const keys = Object.keys(changes ?? {});
  if (!keys.length) return '';
  return keys.map((key) => `${key}: ${changes[key]}`).join(', ');
}

// The board's own query rotation (0x27/0x33/0x34/0x35/0x36/0x3A/0x3B reads and
// their replies) is periodic traffic, not an event worth a log line. Control
// frames - including the ones this page submitted and their acknowledgements -
// are kept. This is a filter on observed frames only; it never claims to be a
// complete audit of the bus.
const PERIODIC_READ_OPS = [0x1a, 0x27, 0x33, 0x34, 0x35, 0x36, 0x37, 0x3a, 0x3b, 0x3c, 0x3d, 0x42, 0x43];

export function isPeriodicQueryFrame(row) {
  if (!row || !Array.isArray(row.data) || !row.data.length) return false;
  if (!PERIODIC_READ_OPS.includes(row.data[0])) return false;
  // Every reply to a polled read is periodic traffic, whatever its length (a
  // 0x36 position reply is seven bytes). A short outgoing frame with a read
  // opcode can only be one of the board's own probes; a longer one is a real
  // command (for example a multi-byte read the queue submitted) and is kept.
  if (row.dir === 'RX') return true;
  return row.data.length <= 3;
}

export function frameText(row) {
  const bytes = (row?.data ?? []).map((b) => Number(b).toString(16).padStart(2, '0').toUpperCase()).join(' ');
  return `${row?.dir ?? '?'} id=0x${Number(row?.canId ?? 0).toString(16).toUpperCase()} ${bytes}`;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

export function exportContext(context) {
  const origin = typeof context?.origin === 'string' ? context.origin : '';
  // The origin is kept without any path, query or credentials: it only answers
  // "which page produced this file".
  let safeOrigin = '';
  try { safeOrigin = new URL(origin).origin; } catch { /* no unparsed URL in export */ }
  return {
    exportedAt: context?.exportedAt ?? new Date().toISOString(),
    origin: safeOrigin,
    logVersion: LOG_VERSION,
    boardId: context?.boardId ?? '',
    motorId: context?.motorId ?? null,
    limits: redactObject(context?.limits),
    motor: redactObject(context?.motor),
    queue: redactObject(context?.queue),
    config: redactObject(context?.config),
    notes: context?.notes ?? '',
  };
}

function eventLine(event) {
  const board = event.boardMs != null ? ` (板端 +${(event.boardMs / 1000).toFixed(1)}s)` : '';
  const request = event.requestId ? ` [${event.requestId}]` : '';
  const uncertain = event.uncertain ? ' (结果未知)' : '';
  const cut = event.truncated ? ' …(板端已截断)' : '';
  return `- ${event.time} · ${event.level.toUpperCase()} · ${event.source} · ${event.event}${request}: ${event.detail}${cut}${board}${uncertain}`;
}

export function exportMarkdown(state, context) {
  const info = exportContext(context);
  const lines = [];
  lines.push('# 电机调试日志导出');
  lines.push('');
  lines.push(`- 导出时间: ${info.exportedAt}`);
  lines.push(`- 来源: ${info.origin || '（未知）'}`);
  lines.push(`- 日志格式版本: ${info.logVersion}`);
  lines.push(`- 板端 bootId: ${state.bootId || '（尚未读取到）'}`);
  lines.push(`- 当前确认限制: ${info.limits ? JSON.stringify(info.limits) : '（未确认）'}`);
  lines.push(`- 选中电机快照: ${info.motor ? JSON.stringify(info.motor) : '（无）'}`);
  if (info.queue) lines.push(`- 队列状态: ${JSON.stringify(info.queue)}`);
  if (info.config) lines.push(`- 配置结果: ${JSON.stringify(info.config)}`);
  lines.push('');
  lines.push('## 说明（这些限制是导出的一部分，不是免责声明）');
  lines.push('- 板端日志是 RAM 环形缓冲，断电重启即丢失，也不是串口控制台全文。');
  lines.push('- 浏览器历史只保存在本浏览器（同源 localStorage），不等于板端完整历史。');
  lines.push('- 只记录实际观察到的 CAN 帧；总线记录缺失会作为缺口标注，不会补写。');
  lines.push('- 从历史恢复的状态仅用于阅读，页面不会据此驱动任何控制或发送。');
  lines.push('');
  lines.push(`## 事件（${state.events.length} 条，最多保留 ${MAX_EVENTS} 条）`);
  if (state.gaps.length) {
    lines.push('');
    lines.push('### 已记录的缺口');
    for (const gap of state.gaps) lines.push(`- ${gap.time}: seq ${gap.from}..${gap.to}（${gap.reason}）`);
  }
  lines.push('');
  for (const event of state.events) {
    lines.push(eventLine(event));
    // The submitted queue program travels with its event so the export answers
    // "which steps were sent" without a second source.
    if (event.program) {
      lines.push('');
      lines.push(`  \`\`\`text${event.programTruncated ? ' (前 8192 字符)' : ''}`);
      for (const line of event.program.split('\n')) lines.push(`  ${line}`);
      lines.push('  ```');
      lines.push('');
    }
  }
  lines.push('');
  lines.push(`## CAN 帧（最近 ${state.trace.length} 帧，最多保留 ${MAX_TRACE_FRAMES} 帧）`);
  lines.push('');
  for (const frame of state.trace) lines.push(`- ${frame.time} · ${frame.text}`);
  return `${lines.join('\n')}\n`;
}

export function exportJson(state, context) {
  const info = exportContext(context);
  return JSON.stringify({
    logVersion: info.logVersion,
    exportedAt: info.exportedAt,
    origin: info.origin,
    boardId: state.bootId || null,
    limits: info.limits,
    motor: info.motor,
    queue: info.queue,
    config: info.config,
    gaps: state.gaps,
    events: state.events,
    trace: state.trace,
  }, null, 2);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

export function emptyState() {
  return {
    version: LOG_VERSION,
    sessionId: '',
    startedAt: 0,
    bootId: '',
    events: [],
    trace: [],
    gaps: [],
    cursor: { bootId: '', seq: 0 },
    boardAvailable: null,   // null = unknown yet
    boardUnavailableLogged: false,
    getFailures: {},
  };
}

function sanitizeEvent(raw) {
  if (!raw || typeof raw !== 'object') return null;
  const at = Number(raw.at);
  if (!Number.isFinite(at)) return null;
  return {
    at,
    time: typeof raw.time === 'string' ? raw.time.slice(0, 24) : '',
    level: LEVELS.includes(raw.level) ? raw.level : 'info',
    source: typeof raw.source === 'string' ? raw.source.slice(0, 12) : 'session',
    event: typeof raw.event === 'string' ? raw.event.slice(0, 40) : 'event',
    detail: clipped(typeof raw.detail === 'string' ? raw.detail : '') || '',
    requestId: typeof raw.requestId === 'string' ? raw.requestId.slice(0, 24) : null,
    uncertain: raw.uncertain === true,
    truncated: raw.truncated === true,
    boardMs: Number.isFinite(Number(raw.boardMs)) ? Number(raw.boardMs) : null,
    program: typeof raw.program === 'string' ? raw.program.slice(0, PROGRAM_TEXT_MAX) : null,
    programTruncated: raw.programTruncated === true,
  };
}

// Long programs are kept for the newest submissions only: the history must not
// be able to fill the origin's storage quota with text.
export const PERSISTED_PROGRAMS = 8;

function boundedEvents(events) {
  let programs = 0;
  const kept = [...events].reverse().map((event) => {
    if (!event.program) return event;
    programs += 1;
    return programs <= PERSISTED_PROGRAMS ? event : {...event, program: null};
  });
  return kept.reverse();
}

export function readPersisted(storage) {
  const empty = emptyState();
  if (!storage) return empty;
  try {
    const text = storage.getItem(LOG_STORAGE_KEY);
    if (!text) return empty;
    const raw = JSON.parse(text);
    if (!raw || typeof raw !== 'object' || raw.version !== LOG_VERSION) return empty;
    const state = empty;
    state.sessionId = typeof raw.sessionId === 'string' ? raw.sessionId.slice(0, 32) : '';
    state.startedAt = Number.isFinite(Number(raw.startedAt)) ? Number(raw.startedAt) : 0;
    state.bootId = typeof raw.bootId === 'string' ? raw.bootId.slice(0, 32) : '';
    state.events = (Array.isArray(raw.events) ? raw.events : []).map(sanitizeEvent).filter(Boolean).slice(-MAX_EVENTS);
    state.trace = (Array.isArray(raw.trace) ? raw.trace : [])
      .filter((frame) => frame && typeof frame === 'object')
      .map((frame) => ({
        at: Number(frame.at) || 0,
        time: typeof frame.time === 'string' ? frame.time.slice(0, 24) : '',
        text: typeof frame.text === 'string' ? frame.text.slice(0, 120) : '',
      }))
      .slice(-MAX_TRACE_FRAMES);
    state.gaps = (Array.isArray(raw.gaps) ? raw.gaps : []).slice(-20).map((gap) => ({
      at: Number(gap?.at) || 0,
      time: typeof gap?.time === 'string' ? gap.time.slice(0, 24) : '',
      from: Number(gap?.from) || 0,
      to: Number(gap?.to) || 0,
      reason: typeof gap?.reason === 'string' ? gap.reason.slice(0, 80) : '',
    }));
    const cursor = raw.cursor && typeof raw.cursor === 'object' ? raw.cursor : {};
    state.cursor = {
      bootId: typeof cursor.bootId === 'string' ? cursor.bootId.slice(0, 32) : '',
      seq: Number.isFinite(Number(cursor.seq)) ? Number(cursor.seq) : 0,
    };
    return state;
  } catch {
    return empty;
  }
}

export function writePersisted(storage, state) {
  if (!storage) return false;
  try {
    storage.setItem(LOG_STORAGE_KEY, JSON.stringify({
      version: LOG_VERSION,
      sessionId: state.sessionId,
      startedAt: state.startedAt,
      bootId: state.bootId,
      events: boundedEvents(state.events.slice(-MAX_EVENTS)),
      trace: state.trace.slice(-MAX_TRACE_FRAMES),
      gaps: state.gaps.slice(-20),
      cursor: state.cursor,
    }));
    return true;
  } catch {
    return false;
  }
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

const clockText = (at) => {
  const date = new Date(at);
  const pad = (value, width = 2) => String(value).padStart(width, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`;
};

export function createLogStore({
  storage = null,
  wallClock = () => Date.now(),
  setTimer = (fn, ms) => setTimeout(fn, ms),
  clearTimer = (handle) => clearTimeout(handle),
} = {}) {
  const state = readPersisted(storage);
  if (!state.sessionId) {
    state.sessionId = Math.random().toString(16).slice(2, 10).toUpperCase();
    state.startedAt = wallClock();
  }
  let listeners = [];
  let saveHandle = null;
  let requestCounter = 0;
  const requestSession = wallClock().toString(36);

  const persistSoon = () => {
    if (saveHandle !== null) return;
    try {
      saveHandle = setTimer(() => { saveHandle = null; writePersisted(storage, state); }, SAVE_DEBOUNCE_MS);
    } catch {
      saveHandle = null;
    }
  };
  const notify = () => {
    for (const listener of [...listeners]) {
      // A subscriber that throws must never break a request or another viewer.
      try { listener(); } catch { /* isolated on purpose */ }
    }
  };

  const push = (entry) => {
    const at = wallClock();
    const event = {
      at,
      time: clockText(at),
      level: LEVELS.includes(entry.level) ? entry.level : 'info',
      source: entry.source ?? 'session',
      event: entry.event ?? 'event',
      detail: typeof entry.detail === 'string' ? entry.detail.slice(0, EVENT_DETAIL_MAX) : '',
      requestId: entry.requestId ?? null,
      uncertain: entry.uncertain === true,
      truncated: entry.truncated === true,
      boardMs: entry.boardMs ?? null,
      // Only a submitted queue program carries this, bounded to 8192 characters.
      program: typeof entry.program === 'string' ? entry.program.slice(0, PROGRAM_TEXT_MAX) : null,
      programTruncated: entry.programTruncated === true,
    };
    state.events.push(event);
    if (state.events.length > MAX_EVENTS) state.events.splice(0, state.events.length - MAX_EVENTS);
    persistSoon();
    notify();
    return event;
  };

  const recordGap = (gap) => {
    const entry = { at: wallClock(), ...gap };
    entry.time = clockText(entry.at);
    state.gaps.push(entry);
    if (state.gaps.length > 20) state.gaps.shift();
  };

  const store = {
    snapshot() {
      return {
        version: state.version,
        sessionId: state.sessionId,
        startedAt: state.startedAt,
        bootId: state.bootId,
        events: [...state.events],
        trace: [...state.trace],
        gaps: [...state.gaps],
        boardAvailable: state.boardAvailable,
        cursor: { ...state.cursor },
        // What was observed for the polled endpoints, kept as small primitive
        // keys: the export can then describe the board even while offline.
        status: { ...(state.statusKey ?? {}) },
        queue: { ...(state.queueKey ?? {}) },
        config: { ...(state.configKey ?? {}) },
        limits: { ...(state.limitsKey ?? {}) },
      };
    },
    subscribe(listener) {
      listeners.push(listener);
      return () => { listeners = listeners.filter((entry) => entry !== listener); };
    },
    flush() {
      if (saveHandle !== null) { try { clearTimer(saveHandle); } catch { /* ignore */ } saveHandle = null; }
      return writePersisted(storage, state);
    },

    // ---- HTTP ------------------------------------------------------------
    beginRequest({ method, path, params }) {
      if (method !== 'POST') return null;
      const requestId = `${requestSession}-r${++requestCounter}`;
      const safe = sanitizeParams(path, params);
      const program = typeof safe.program === 'string' ? safe.program : null;
      const programTruncated = safe.programTruncated === true;
      // The detail line summarises the program; the text itself travels with the
      // event so the operator can see exactly which steps were submitted.
      const shown = program === null ? safe : {...safe, program: programSummary(program)};
      push({
        source: 'http', level: 'info', event: 'request.submitted', requestId,
        detail: `POST ${path}${formatParams(shown) ? ` ${formatParams(shown)}` : ''} · 已提交，等待板端应答（提交不等于执行）`
          + `${programTruncated ? '（程序超过 8192 字符，只保留前 8192 字符）' : ''}`,
        program,
        programTruncated,
      });
      return requestId;
    },
    finishRequest(requestId, { path, status, body, durationMs }) {
      if (!requestId) return;
      const summary = summarizeBody(body);
      const queued = status === 202;
      push({
        source: 'http',
        level: status >= 400 ? 'warn' : 'info',
        event: 'http.result',
        requestId,
        detail: `${path} → HTTP ${status} (${durationMs}ms)${queued ? ' · 已入队（202，仅代表板端已接受，不代表驱动器应答或动作完成）' : ''}`
          + `${summary.message ? ` · message=${summary.message}` : ''}`
          + `${summary.error ? ` · error=${summary.error}` : ''}`
          + `${summary.line != null ? ` · line=${summary.line}` : ''}`,
      });
    },
    failRequest(requestId, { path, error, uncertain, status, durationMs }) {
      if (!requestId) return;
      push({
        source: 'http',
        level: 'error',
        event: 'request.error',
        requestId,
        uncertain: uncertain === true,
        detail: `${path} → ${status ? `HTTP ${status}` : error} (${durationMs}ms)`
          + `${uncertain ? ' · 结果未知：本页不会自动重发' : ''}`,
      });
    },

    /**
     * One line per GET *failure* (never one per poll) and one line per observed
     * change of the polled state. `/api/logs` is handled by noteBoardLogResult.
     */
    noteGet({ path, ok, status, body, error, uncertain }) {
      const failures = state.getFailures;
      const previous = failures[path];
      if (ok) {
        if (previous) {
          delete failures[path];
          push({ source: 'http', level: 'info', event: 'http.recovered', detail: `${path} 恢复正常读取` });
        }
      } else if (!previous) {
        failures[path] = { status: status ?? 0, error: error ?? '' };
        push({
          source: 'http', level: 'warn', event: 'http.failed', uncertain: uncertain === true,
          detail: `${path} 读取失败：${status ? `HTTP ${status}` : error}${uncertain ? ' · 结果未知' : ''}（重复失败不再逐次记录）`,
        });
      }
      return this;
    },
    // A payload without any known field (malformed or from another firmware) is
    // ignored entirely: it is neither a change nor a new baseline.
    noteStatus(status) {
      const next = statusKey(status);
      if (!Object.keys(next).length) return this;
      const changed = diffFields(state.statusKey, next);
      state.statusKey = next;
      if (state.statusSeen && Object.keys(changed).length) {
        push({ source: 'status', level: changed.fault && changed.fault !== 'none' ? 'error' : 'info', event: 'status.changed', detail: formatChanges(changed) });
      }
      state.statusSeen = true;
      return this;
    },
    noteQueue(queue) {
      const next = queueKey(queue);
      if (!Object.keys(next).length) return this;
      const changed = diffFields(state.queueKey, next);
      state.queueKey = next;
      if (state.queueSeen && Object.keys(changed).length) {
        push({ source: 'queue', level: changed.state === 'failed' ? 'error' : 'info', event: 'queue.changed', detail: formatChanges(changed) });
      }
      state.queueSeen = true;
      return this;
    },
    noteConfig(config) {
      const next = configKey(config);
      if (!Object.keys(next).length) return this;
      const changed = diffFields(state.configKey, next);
      state.configKey = next;
      if (state.configSeen && Object.keys(changed).length) {
        push({ source: 'config', level: 'info', event: 'config.changed', detail: formatChanges(changed) });
      }
      state.configSeen = true;
      return this;
    },
    noteLimits(limits) {
      const next = limitsKey(limits);
      if (!Object.keys(next).length) return this;
      const changed = diffFields(state.limitsKey, next);
      state.limitsKey = next;
      if (state.limitsSeen && Object.keys(changed).length) {
        push({ source: 'limits', level: 'info', event: 'limits.confirmed', detail: formatChanges(changed) });
      }
      state.limitsSeen = true;
      return this;
    },

    // ---- board log -------------------------------------------------------
    /**
     * `available` is what the UI uses to slow the poll down after a 404; the
     * transition is logged once, and a missing endpoint is never reported as the
     * device being offline.
     */
    noteBoardLogResult({ ok, status, error }) {
      if (ok) {
        if (state.boardAvailable === false) {
          push({ source: 'board', level: 'info', event: 'board.recovered', detail: '板端日志接口恢复可读' });
        }
        state.boardAvailable = true;
        state.boardUnavailableLogged = false;
        return { available: true };
      }
      const missing = status === 404;
      if (missing && !state.boardUnavailableLogged) {
        state.boardUnavailableLogged = true;
        push({
          source: 'board', level: 'warn', event: 'board.unavailable',
          detail: '板端没有 /api/logs 接口（固件未更新）：日志页只能显示本机历史，设备状态与其它接口不受影响',
        });
      } else if (!missing && state.boardAvailable !== false) {
        push({ source: 'board', level: 'warn', event: 'board.failed', detail: `板端日志读取失败：${status ? `HTTP ${status}` : error}` });
      }
      state.boardAvailable = false;
      return { available: false };
    },
    /** Merges one GET /api/logs payload: dedup by bootId+seq, restart and gap. */
    mergeBoardLog(payload) {
      const parsed = parseBoardLog(payload);
      if (!parsed.ok) {
        this.noteBoardLogResult({ ok: false, status: 0, error: parsed.error });
        return { accepted: false, reason: parsed.error };
      }
      this.noteBoardLogResult({ ok: true });
      const delta = boardDelta(parsed, state.cursor);
      if (delta.restarted) {
        if (state.cursor.bootId) {
          push({
            source: 'board', level: 'warn', event: 'board.restart',
            detail: `板端重启：bootId ${state.cursor.bootId} → ${parsed.bootId}（RAM 环形缓冲已清空，历史不再可用）`,
          });
          recordGap({ from: 0, to: 0, reason: '板端重启，旧 bootId 的日志不复存在' });
        }
        state.cursor = { bootId: parsed.bootId, seq: 0 };
      }
      state.bootId = parsed.bootId;
      if (delta.bounded && state.lastOmitted !== delta.omitted) {
        // Reported once per distinct count, never as a false gap.
        state.lastOmitted = delta.omitted;
        push({
          source: 'board', level: 'info', event: 'board.bounded',
          detail: `板端响应长度受限：本次只返回最近 ${parsed.events.length} 条，另有 ${delta.omitted} 条未随本次返回（不是被覆盖）`,
        });
      }
      if (delta.gap) {
        recordGap({ from: delta.gap.from, to: delta.gap.to, reason: '板端环形缓冲已覆盖这些序号' });
        push({
          source: 'board', level: 'warn', event: 'board.gap',
          detail: `板端日志缺口：seq ${delta.gap.from}..${delta.gap.to} 已被环形缓冲覆盖，无法补齐`,
        });
      }
      for (const event of delta.fresh) {
        state.cursor.seq = Math.max(state.cursor.seq, event.seq);
        push({
          source: 'board',
          level: event.level,
          event: event.event,
          detail: event.detail,
          truncated: event.truncated,
          boardMs: event.atMs,
        });
      }
      state.cursor.seq = Math.max(state.cursor.seq, parsed.sequence);
      persistSoon();
      notify();
      return { accepted: true, restarted: delta.restarted, gap: delta.gap, imported: delta.fresh.length };
    },

    // ---- CAN -------------------------------------------------------------
    /** Keeps notable frames only: the periodic query rotation stays out. */
    recordTraceRows(rows) {
      let added = 0;
      for (const row of rows ?? []) {
        if (isPeriodicQueryFrame(row)) continue;
        state.trace.push({ at: row.at ?? wallClock(), time: row.time ?? clockText(row.at ?? wallClock()), text: frameText(row) });
        added += 1;
      }
      if (!added) return 0;
      if (state.trace.length > MAX_TRACE_FRAMES) state.trace.splice(0, state.trace.length - MAX_TRACE_FRAMES);
      push({ source: 'can', level: 'debug', event: 'can.frames', detail: `记录 ${added} 个非轮询 CAN 帧（仅实际观察到的帧，缺失以缺口标注）` });
      persistSoon();
      return added;
    },
    noteTraceGap(detail) {
      recordGap({ from: 0, to: 0, reason: detail });
      push({ source: 'can', level: 'warn', event: 'trace.gap', detail });
    },
    noteTraceRestart() {
      push({ source: 'can', level: 'warn', event: 'trace.restart', detail: '板端计数器回退：总线记录已重新开始，之前的帧不再连续' });
    },

    // ---- history ---------------------------------------------------------
    /**
     * Clears the local history only. The board cursor is kept on purpose so a
     * clear does not immediately re-import the board ring that is still there.
     */
    clear() {
      state.events = [];
      state.trace = [];
      state.gaps = [];
      state.getFailures = {};
      state.statusSeen = false;
      state.queueSeen = false;
      state.configSeen = false;
      state.limitsSeen = false;
      push({ source: 'session', level: 'info', event: 'log.cleared', detail: '已清空本机日志历史（不影响电机草稿与板端 RAM 日志游标）' });
      persistSoon();
      return this;
    },
    add: push,
  };
  return store;
}

// The page-wide singleton. Storage access is defensive: with localStorage denied
// the store still works for the current page and simply cannot be persisted.
export const logStore = createLogStore({ storage: (() => {
  try {
    const storage = globalThis.localStorage;
    if (!storage) return null;
    const probe = '__babytech_log_probe__';
    storage.setItem(probe, '1');
    storage.removeItem(probe);
    return storage;
  } catch {
    return null;
  }
})() });
