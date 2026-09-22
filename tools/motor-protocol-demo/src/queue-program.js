// Queue program DSL: parse, validate and preview the text the board executes.
//
// Pure module: no React, no network, no timers. It settles **syntax and numeric
// framing** only — units, ranges that come from the wire format / firmware
// constants, unknown verbs, raw frame bounds. Everything that depends on the
// board's editable policy (DebugLimits) is left to the board, which validates
// the whole program before it sends a single CAN frame.
//
// The module is also the single source of truth for the editor's help list, the
// action builder and the readable preview, so the three can never drift apart.
//
// One action per line, '#' starts a comment, command and unit are
// case-insensitive, optional arguments are trailing only.

export const QUEUE_LIMITS = {
  maxActions: 64, // board: max 64 nonblank actions
  maxTextBytes: 8192, // board: max 8192 text bytes
  repeatMin: 1,
  repeatMax: 1000,
  addressMin: 1,
  addressMax: 255,
  homeModeMax: 5, // manual V1.0.5 p61: homing modes 00-05
  waitMinMs: 0,
  waitMaxMs: 3600000,
  durationMinMs: 1,
  durationMaxMs: 3600000,
  hexMinBytes: 3, // includes address + checksum
  hexMaxBytes: 30,
  canDataMaxBytes: 8,
  canExtendedMaxId: 0x1fffffff,
  canStandardMaxId: 0x7ff,
  // Firmware constants (MotionCore.h) and manual ceilings — not board policy.
  tenthsPerDegree: 10,
  degreesPerRev: 360,
  minAngleDeg: 0.1,
  minSpeedRpm: 0.1,
  maxSpeedRpm: 3000,
  minAccelRpmS: 1,
  maxAccelRpmS: 65535,
  minCurrentMa: 100,
  maxCurrentMa: 5000,
};

const ID_SPAN = `${QUEUE_LIMITS.addressMin}..${QUEUE_LIMITS.addressMax}`;

const NUMBER_RE = /^-?\d+(?:\.\d+)?$/;
const INTEGER_RE = /^-?\d+$/;
const ADDRESS_RE = /^\d+$/;
const HEX_BYTE_RE = /^[0-9a-fA-F]{2}$/;
const CAN_ID_RE = /^(?:0x)?([0-9a-fA-F]{1,8})$/;

/**
 * Every verb the board accepts, with the short form and the defaults that the
 * help list has to show verbatim. `args` doubles as the action builder's field
 * list, in wire order: the optional ones are trailing only.
 */
export const QUEUE_VERBS = [
  {
    verb: 'enable',
    label: '使能',
    usage: 'enable ID',
    shortForm: 'enable 1',
    defaults: '地址必填，没有隐式使能',
    note: '等待驱动器真实的 F3 应答。程序不会替任何电机隐式使能，需要运动就先写 enable。',
    args: [{ key: 'id', label: '电机地址', kind: 'address', default: '1' }],
  },
  {
    verb: 'disable',
    label: '失能',
    usage: 'disable ID',
    shortForm: 'disable 1',
    defaults: '地址必填',
    note: '关闭使能：等待应答，并确认电机真实静止。示例程序故意不写 disable：定时力矩／速度动作会自行停止，正常停止也保留使能；确认要断电或人工干预时再加这一行。',
    args: [{ key: 'id', label: '电机地址', kind: 'address', default: '1' }],
  },
  {
    verb: 'move',
    label: '相对运动',
    usage: 'move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]]',
    shortForm: 'move 1 90',
    defaults: '单位 deg · RPM 30 · 加减速 60 · 电流 800 mA',
    note: 'VALUE 带符号表示方向；rev = 360°；mm 用该地址已保存的 mm/rev 旋转距离换算。等待受监督的 CD 完成。',
    args: [
      { key: 'id', label: '电机地址', kind: 'address', default: '1' },
      { key: 'value', label: '行程', kind: 'number', unit: 'deg|rev|mm', default: '90' },
      { key: 'unit', label: '单位', kind: 'unit', default: 'deg' },
      { key: 'rpm', label: '转速', kind: 'number', unit: 'RPM', default: '30' },
      { key: 'accel', label: '加速度', kind: 'number', unit: 'RPM/s', default: '60' },
      { key: 'decel', label: '减速度', kind: 'number', unit: 'RPM/s', default: '60' },
      { key: 'current', label: '电流上限', kind: 'number', unit: 'mA', default: '800' },
    ],
  },
  {
    verb: 'home',
    label: '回零',
    usage: 'home ID [MODE]',
    shortForm: 'home 2',
    defaults: '模式 0（单圈就近）',
    note: '触发 9A 回零并等待板端的监督结果；12/22 表示已在零点或限位已触发、电机未动，不等于回零完成。',
    args: [
      { key: 'id', label: '电机地址', kind: 'address', default: '2' },
      { key: 'mode', label: '回零模式', kind: 'number', unit: '0..5', default: '0' },
    ],
  },
  {
    verb: 'torque',
    label: '限速力矩',
    usage: 'torque ID SIGNED_MA DURATION_MS [MAX_RPM [RAMP_MA_S]]',
    shortForm: 'torque 3 -300 1500',
    defaults: '限速 30 RPM · 斜率 1000 mA/s',
    note: 'C5 限速力矩：SIGNED_MA 带符号决定方向，到时停止并等待静止。力矩以 mA 下发，界面不使用 Nm。',
    args: [
      { key: 'id', label: '电机地址', kind: 'address', default: '3' },
      { key: 'currentMa', label: '力矩电流', kind: 'number', unit: 'mA（带符号）', default: '300', signed: true },
      { key: 'durationMs', label: '持续时间', kind: 'number', unit: 'ms', default: '1500' },
      { key: 'maxRpm', label: '限速', kind: 'number', unit: 'RPM', default: '30' },
      { key: 'rampMaS', label: '电流斜率', kind: 'number', unit: 'mA/s', default: '1000' },
    ],
  },
  {
    verb: 'velocity',
    label: '速度',
    usage: 'velocity ID SIGNED_RPM DURATION_MS [ACCEL [CURRENT]]',
    shortForm: 'velocity 1 60 2000',
    defaults: '加速度 60 RPM/s · 电流 800 mA',
    note: 'C6 限流速度：SIGNED_RPM 带符号决定方向，到时停止并等待静止。',
    args: [
      { key: 'id', label: '电机地址', kind: 'address', default: '1' },
      { key: 'rpm', label: '转速', kind: 'number', unit: 'RPM（带符号）', default: '60', signed: true },
      { key: 'durationMs', label: '持续时间', kind: 'number', unit: 'ms', default: '2000' },
      { key: 'accel', label: '加速度', kind: 'number', unit: 'RPM/s', default: '60' },
      { key: 'current', label: '电流上限', kind: 'number', unit: 'mA', default: '800' },
    ],
  },
  {
    verb: 'stop',
    label: '停止',
    usage: 'stop ID',
    shortForm: 'stop 1',
    defaults: '地址必填',
    note: '停止并等待真实静止反馈；保留使能状态（要真正断电请再写一行 disable）。',
    args: [{ key: 'id', label: '电机地址', kind: 'address', default: '1' }],
  },
  {
    verb: 'wait',
    label: '等待',
    usage: 'wait MS',
    shortForm: 'wait 200',
    defaults: `整数 ${QUEUE_LIMITS.waitMinMs}..${QUEUE_LIMITS.waitMaxMs} ms`,
    note: '非阻塞等待，只推迟后续动作。',
    args: [{ key: 'ms', label: '等待时间', kind: 'number', unit: 'ms', integer: true, default: '200' }],
  },
  {
    verb: 'hex',
    label: '原始逻辑指令',
    usage: 'hex AA BB ...',
    shortForm: 'hex 01 F3 AB 01 00 6B',
    defaults: `${QUEUE_LIMITS.hexMinBytes}..${QUEUE_LIMITS.hexMaxBytes} 字节，原样下发`,
    note: '绕过功能码白名单的逻辑指令：字节原样保留、由驱动分包发送。板端不做运动监督、不推断完成、步骤结束后不会自动停止。',
    args: [{ key: 'bytes', label: '字节', kind: 'raw', unit: '十六进制', default: '01 F3 AB 01 00 6B' }],
  },
  {
    verb: 'can',
    label: '原始 CAN 帧',
    usage: 'can ext|std IDHEX BYTE...',
    shortForm: 'can ext 100 9A 00 00',
    defaults: `ID 十六进制，扩展帧 ≤ ${QUEUE_LIMITS.canExtendedMaxId.toString(16).toUpperCase()}、标准帧 ≤ 7FF，0..${QUEUE_LIMITS.canDataMaxBytes} 字节数据`,
    note: '发送一条真实 CAN 数据帧：不重写、不追加 6B、不重试、不推断完成。',
    args: [{ key: 'frame', label: '帧', kind: 'raw', unit: 'ext|std ID 数据', default: 'ext 100 9A 00 00' }],
  },
];

const VERB_BY_NAME = new Map(QUEUE_VERBS.map((entry) => [entry.verb, entry]));

export const getVerbDefinition = (verb) => VERB_BY_NAME.get(String(verb ?? '').toLowerCase()) ?? null;

export const QUEUE_VERB_NAMES = QUEUE_VERBS.map((entry) => entry.verb);

/** Homing modes, labels straight from the manual (V1.0.5 p61-p64). */
export const QUEUE_HOME_MODES = [
  { value: 0, label: '单圈就近' },
  { value: 1, label: '单圈方向' },
  { value: 2, label: '无限位碰撞' },
  { value: 3, label: '限位' },
  { value: 4, label: '绝对零点' },
  { value: 5, label: '上次掉电位置' },
];

export const homeModeLabel = (mode) =>
  QUEUE_HOME_MODES.find((entry) => entry.value === Number(mode))?.label ?? `未知模式 ${mode}`;

export const QUEUE_UNITS = ['deg', 'rev', 'mm'];

/** Result of looking up one ID's confirmed mm/rev rotation distance. */
export function distanceLookup(distances, id) {
  if (!distances) return { state: 'unknown', value: null };
  const value = distances instanceof Map ? distances.get(id) : distances[id];
  if (value === undefined) return { state: 'unknown', value: null };
  if (value === null) return { state: 'none', value: null };
  const numeric = Number(value);
  if (!Number.isFinite(numeric) || numeric <= 0) return { state: 'none', value: null };
  return { state: 'known', value: numeric };
}

// ---------------------------------------------------------------------------
// Number tokens
// ---------------------------------------------------------------------------

function readNumber(token, spec, fail) {
  const integer = spec.integer === true;
  if (!(integer ? INTEGER_RE : NUMBER_RE).test(token)) {
    fail(`「${spec.label}」只接受十进制数字${integer ? '（整数）' : ''}，不接受 + 号、0x、科学计数法、表达式或空值：${token}`);
    return null;
  }
  const value = Number(token);
  if (!Number.isFinite(value)) {
    fail(`「${spec.label}」必须是有限数字`);
    return null;
  }
  // Signed fields carry their sign separately: the range applies to the magnitude.
  const subject = spec.signed ? Math.abs(value) : value;
  const min = spec.signed ? spec.absMin : spec.min;
  const max = spec.signed ? spec.absMax : spec.max;
  if (min != null && subject < min) {
    fail(`「${spec.label}」不能小于 ${min}${spec.unit ? ` ${spec.unit}` : ''}（协议与固件边界，不做截断）`);
    return null;
  }
  if (max != null && subject > max) {
    fail(`「${spec.label}」不能超过 ${max}${spec.unit ? ` ${spec.unit}` : ''}（协议与固件边界，不做截断）`);
    return null;
  }
  return value;
}

const addressSpec = { label: '电机地址', integer: true, min: QUEUE_LIMITS.addressMin, max: QUEUE_LIMITS.addressMax };

function readAddress(token, fail) {
  if (!ADDRESS_RE.test(token)) {
    fail(`电机地址必须是十进制整数（${ID_SPAN}）：${token}`);
    return null;
  }
  return readNumber(token, addressSpec, fail);
}

const SPEED = { label: '转速', min: QUEUE_LIMITS.minSpeedRpm, max: QUEUE_LIMITS.maxSpeedRpm, unit: 'RPM' };
const ACCEL = { label: '加减速度', integer: true, min: QUEUE_LIMITS.minAccelRpmS, max: QUEUE_LIMITS.maxAccelRpmS, unit: 'RPM/s' };
const CURRENT = { label: '电流上限', integer: true, min: QUEUE_LIMITS.minCurrentMa, max: QUEUE_LIMITS.maxCurrentMa, unit: 'mA' };
const DURATION = { label: '持续时间', integer: true, min: QUEUE_LIMITS.durationMinMs, max: QUEUE_LIMITS.durationMaxMs, unit: 'ms' };
const WAIT_MS = { label: '等待时间', integer: true, min: QUEUE_LIMITS.waitMinMs, max: QUEUE_LIMITS.waitMaxMs, unit: 'ms' };
const HOME_MODE = { label: '回零模式', integer: true, min: 0, max: QUEUE_LIMITS.homeModeMax };

// ---------------------------------------------------------------------------
// One line -> one action
// ---------------------------------------------------------------------------

function parseHexLine(tokens, fail) {
  const bytes = [];
  for (const token of tokens) {
    if (!HEX_BYTE_RE.test(token)) {
      fail(`原始逻辑指令的每个字节都需要两位十六进制字符：${token}`);
      return null;
    }
    bytes.push(parseInt(token, 16));
  }
  if (bytes.length < QUEUE_LIMITS.hexMinBytes || bytes.length > QUEUE_LIMITS.hexMaxBytes) {
    fail(`原始逻辑指令需要 ${QUEUE_LIMITS.hexMinBytes}..${QUEUE_LIMITS.hexMaxBytes} 字节（含地址与固定校验 6B），当前 ${bytes.length} 字节`);
    return null;
  }
  return { bytes };
}

function parseCanLine(tokens, fail) {
  // ext/std is part of the command, so it follows the same case-insensitive rule.
  const [rawForm, idToken, ...data] = tokens;
  const form = String(rawForm ?? '').toLowerCase();
  if (form !== 'ext' && form !== 'std') {
    fail(`原始 CAN 帧的第二个词必须是 ext（扩展帧）或 std（标准帧）：${rawForm || '（缺少）'}`);
    return null;
  }
  if (idToken === undefined) {
    fail('原始 CAN 帧缺少 ID（十六进制，扩展帧可带 0x 前缀）');
    return null;
  }
  const idMatch = CAN_ID_RE.exec(idToken);
  if (!idMatch) {
    fail(`CAN ID 只能是十六进制字符（可带 0x 前缀，最多 8 位）：${idToken}`);
    return null;
  }
  const canId = parseInt(idMatch[1], 16);
  const limit = form === 'ext' ? QUEUE_LIMITS.canExtendedMaxId : QUEUE_LIMITS.canStandardMaxId;
  if (canId > limit) {
    fail(`${form === 'ext' ? '扩展帧' : '标准帧'} ID 不能超过 0x${limit.toString(16).toUpperCase()}：${idToken}`);
    return null;
  }
  if (data.length > QUEUE_LIMITS.canDataMaxBytes) {
    fail(`一条 CAN 数据帧最多 ${QUEUE_LIMITS.canDataMaxBytes} 字节，当前 ${data.length} 字节`);
    return null;
  }
  const bytes = [];
  for (const token of data) {
    if (!HEX_BYTE_RE.test(token)) {
      fail(`CAN 数据字节需要两位十六进制字符：${token}`);
      return null;
    }
    bytes.push(parseInt(token, 16));
  }
  return { extended: form === 'ext', canId, data: bytes };
}

function parseMove(tokens, fail) {
  const [idToken, valueToken, ...rest] = tokens;
  if (idToken === undefined) {
    fail('缺少电机地址（用法：move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]]）');
    return null;
  }
  if (valueToken === undefined) {
    fail('缺少行程 VALUE（用法：move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]]）');
    return null;
  }
  const id = readAddress(idToken, fail);
  const value = readNumber(valueToken, { label: '行程', min: QUEUE_LIMITS.minAngleDeg, signed: true, absMin: QUEUE_LIMITS.minAngleDeg }, fail);
  if (id == null || value == null) return null;
  let unit = 'deg';
  if (rest.length && QUEUE_UNITS.includes(rest[0].toLowerCase())) unit = rest.shift().toLowerCase();
  const optional = [SPEED, ACCEL, ACCEL, CURRENT];
  const keys = ['rpm', 'accel', 'decel', 'current'];
  const defaults = { rpm: 30, accel: 60, decel: 60, current: 800 };
  const values = { ...defaults };
  if (rest.length > optional.length) {
    fail(`相对运动最多 4 个可选参数（RPM ACCEL DECEL CURRENT），多出 ${rest.length - optional.length} 个`);
    return null;
  }
  for (let index = 0; index < rest.length; index += 1) {
    const parsed = readNumber(rest[index], { ...optional[index], label: ['转速', '加速度', '减速度', '电流上限'][index] }, fail);
    if (parsed == null) return null;
    values[keys[index]] = parsed;
  }
  return { id, value, unit, ...values };
}

function parseTorque(tokens, fail) {
  const [idToken, currentToken, durationToken, ...rest] = tokens;
  if (idToken === undefined || currentToken === undefined || durationToken === undefined) {
    fail('用法：torque ID SIGNED_MA DURATION_MS [MAX_RPM [RAMP_MA_S]]');
    return null;
  }
  const id = readAddress(idToken, fail);
  const currentMa = readNumber(currentToken, { label: '力矩电流', integer: true, signed: true, absMin: 1, absMax: QUEUE_LIMITS.maxCurrentMa, unit: 'mA' }, fail);
  const durationMs = readNumber(durationToken, DURATION, fail);
  if (id == null || currentMa == null || durationMs == null) return null;
  if (rest.length > 2) {
    fail(`限速力矩最多 2 个可选参数（MAX_RPM RAMP_MA_S），多出 ${rest.length - 2} 个`);
    return null;
  }
  const maxRpm = rest.length > 0
    ? readNumber(rest[0], { ...SPEED, label: '限速' }, fail)
    : 30;
  const rampMaS = rest.length > 1
    ? readNumber(rest[1], { label: '电流斜率', integer: true, min: 0, max: QUEUE_LIMITS.maxAccelRpmS, unit: 'mA/s' }, fail)
    : 1000;
  if (maxRpm == null || rampMaS == null) return null;
  return { id, currentMa, durationMs, maxRpm, rampMaS };
}

function parseVelocity(tokens, fail) {
  const [idToken, rpmToken, durationToken, ...rest] = tokens;
  if (idToken === undefined || rpmToken === undefined || durationToken === undefined) {
    fail('用法：velocity ID SIGNED_RPM DURATION_MS [ACCEL [CURRENT]]');
    return null;
  }
  const id = readAddress(idToken, fail);
  const rpm = readNumber(rpmToken, { ...SPEED, label: '转速', signed: true, absMin: QUEUE_LIMITS.minSpeedRpm, absMax: QUEUE_LIMITS.maxSpeedRpm }, fail);
  const durationMs = readNumber(durationToken, DURATION, fail);
  if (id == null || rpm == null || durationMs == null) return null;
  if (rest.length > 2) {
    fail(`速度指令最多 2 个可选参数（ACCEL CURRENT），多出 ${rest.length - 2} 个`);
    return null;
  }
  const accel = rest.length > 0 ? readNumber(rest[0], { ...ACCEL, label: '加速度' }, fail) : 60;
  const current = rest.length > 1 ? readNumber(rest[1], CURRENT, fail) : 800;
  if (accel == null || current == null) return null;
  return { id, rpm, durationMs, accel, current };
}

/** Parse one already-tokenised action line. Returns null when `fail` was called. */
function parseTokens(verb, tokens, fail) {
  switch (verb) {
    case 'enable':
    case 'disable':
    case 'stop': {
      if (tokens.length !== 1) {
        fail(`用法：${verb} ID（只接受一个电机地址）`);
        return null;
      }
      const id = readAddress(tokens[0], fail);
      return id == null ? null : { id };
    }
    case 'wait': {
      if (tokens.length !== 1) {
        fail('用法：wait MS（整数 0..3600000）');
        return null;
      }
      const ms = readNumber(tokens[0], WAIT_MS, fail);
      return ms == null ? null : { ms };
    }
    case 'home': {
      if (tokens.length < 1 || tokens.length > 2) {
        fail('用法：home ID [MODE]（模式 0..5，默认 0）');
        return null;
      }
      const id = readAddress(tokens[0], fail);
      const mode = tokens.length === 2 ? readNumber(tokens[1], HOME_MODE, fail) : 0;
      if (id == null || mode == null) return null;
      return { id, mode };
    }
    case 'move': return parseMove(tokens, fail);
    case 'torque': return parseTorque(tokens, fail);
    case 'velocity': return parseVelocity(tokens, fail);
    case 'hex': {
      const parsed = parseHexLine(tokens, fail);
      return parsed;
    }
    case 'can': {
      const parsed = parseCanLine(tokens, fail);
      return parsed;
    }
    default:
      fail(`未知指令「${verb}」；可用指令：${QUEUE_VERB_NAMES.join(' / ')}`);
      return null;
  }
}

// ---------------------------------------------------------------------------
// Program -> actions
// ---------------------------------------------------------------------------

const utf8Length = (text) => (typeof TextEncoder === 'function'
  ? new TextEncoder().encode(text).length
  : Buffer.byteLength(text, 'utf8'));

/**
 * Parse a whole program. Never throws: syntax problems come back as
 * `errors: [{line, message}]` with the 1-based source line, so the editor can
 * point at the line the board would have rejected.
 */
export function parseProgram(text) {
  const source = String(text ?? '');
  const errors = [];
  const actions = [];
  const lines = source.split(/\r\n|\r|\n/);
  lines.forEach((rawLine, index) => {
    const line = index + 1;
    const withoutComment = rawLine.split('#')[0].trim();
    if (!withoutComment) return;
    const tokens = withoutComment.split(/\s+/);
    const verb = tokens[0].toLowerCase();
    const fail = (message) => errors.push({ line, message });
    const body = parseTokens(verb, tokens.slice(1), fail);
    if (body) actions.push({ line, verb, ...body });
  });
  return { actions, errors, lineCount: lines.length };
}

/** Repeat count from the form: 1..1000, integers only. */
export function checkRepeat(raw) {
  const text = String(raw ?? '').trim();
  if (!INTEGER_RE.test(text)) return { ok: false, error: '重复次数只能是 1..1000 的整数' };
  const value = Number(text);
  if (value < QUEUE_LIMITS.repeatMin || value > QUEUE_LIMITS.repeatMax) {
    return { ok: false, error: `重复次数需在 ${QUEUE_LIMITS.repeatMin}..${QUEUE_LIMITS.repeatMax} 之间` };
  }
  return { ok: true, value };
}

// ---------------------------------------------------------------------------
// Preview
// ---------------------------------------------------------------------------

const round = (value, decimals = 0) => Number(value.toFixed(decimals));
const signed = (value) => `${value > 0 ? '+' : ''}${value}`;
const num = (value) => String(round(value, 4));
const degrees = (value) => `${value > 0 ? '+' : ''}${round(value, 4)}°`;
const tenths = (deg) => Math.round(Math.abs(deg) * QUEUE_LIMITS.tenthsPerDegree);

/**
 * Angle in degrees for a distance action, plus the tenths the wire field will
 * carry. `mm` needs the ID's confirmed mm/rev distance; without it the caller
 * must not guess a conversion.
 */
export function actionAngleDegrees(action, distances) {
  if (action.verb !== 'move') return { ok: false, reason: 'not_a_move' };
  const lookup = distanceLookup(distances, action.id);
  if (action.unit === 'deg') return { ok: true, deg: action.value, lookup };
  if (action.unit === 'rev') return { ok: true, deg: action.value * QUEUE_LIMITS.degreesPerRev, lookup };
  if (lookup.state === 'known') {
    return { ok: true, deg: (action.value / lookup.value) * QUEUE_LIMITS.degreesPerRev, lookup };
  }
  return { ok: false, reason: lookup.state === 'none' ? 'distance_missing' : 'distance_unknown', lookup };
}

/**
 * Readable one-line meaning of an action, with the exact numbers that will be
 * sent. `warnings` are honest caveats (raw steps, unchecked mm) — they never
 * block submission, they only stop the preview from over-promising.
 */
export function previewAction(action, { distances = null } = {}) {
  const warnings = [];
  switch (action.verb) {
    case 'enable':
      return { summary: `电机 ${action.id}：使能（等待真实 F3 应答）`, warnings };
    case 'disable':
      return { summary: `电机 ${action.id}：关闭使能（等待应答并确认真实静止）`, warnings };
    case 'stop':
      return { summary: `电机 ${action.id}：停止（等待静止反馈，保留使能）`, warnings };
    case 'wait':
      return { summary: `等待 ${action.ms} ms（非阻塞）`, warnings };
    case 'home':
      return {
        summary: `电机 ${action.id}：回零 · 模式 ${action.mode} ${homeModeLabel(action.mode)}（等待受监督的 9A 结果）`,
        warnings,
      };
    case 'move': {
      const angle = actionAngleDegrees(action, distances);
      const counts = angle.ok ? tenths(angle.deg) : null;
      const head = `电机 ${action.id}：相对运动 ${signed(action.value)} ${action.unit}`;
      let detail = `${num(action.rpm)} RPM · 加减速 ${num(action.accel)}/${num(action.decel)} RPM/s · 电流上限 ${action.current} mA`;
      if (angle.ok) {
        if (action.unit === 'mm') {
          detail = `旋转距离 ${num(angle.lookup.value)} mm/rev → ${round(angle.deg, 3)}°（${counts} × 0.1°）· ${detail}`;
        } else {
          detail = `${degrees(angle.deg)}（${counts} × 0.1°）· ${detail}`;
        }
        if (counts < 1) {
          warnings.push(`行程换算后不足 1 个 0.1° 计数（${counts}），板端会以“取整为零”拒绝。`);
        }
      } else if (angle.reason === 'distance_missing') {
        warnings.push(`电机 ${action.id} 尚未保存 mm/rev 旋转距离：mm 行程无法换算角度，板端会拒绝这一步。`);
        detail = `旋转距离未知 · ${detail}`;
      } else {
        warnings.push(`尚未从板端读取电机 ${action.id} 的 mm/rev 旋转距离，本页不推测换算结果。`);
        detail = `旋转距离未读取 · ${detail}`;
      }
      return { summary: head, detail, warnings, counts, deg: angle.ok ? angle.deg : null };
    }
    case 'torque':
      return {
        summary: `电机 ${action.id}：限速力矩 ${signed(action.currentMa)} mA · ${action.durationMs} ms · 限速 ${num(action.maxRpm)} RPM · 斜率 ${action.rampMaS} mA/s`,
        detail: `到时停止并等待静止；力矩以 mA 下发（不使用 Nm）`,
        warnings,
      };
    case 'velocity':
      return {
        summary: `电机 ${action.id}：速度 ${signed(action.rpm)} RPM · ${action.durationMs} ms`,
        detail: `加速度 ${num(action.accel)} RPM/s · 电流上限 ${action.current} mA · 到时停止并等待静止`,
        warnings,
      };
    case 'hex': {
      const hex = action.bytes.map((byte) => byte.toString(16).toUpperCase().padStart(2, '0')).join(' ');
      if (action.bytes[action.bytes.length - 1] !== 0x6b || action.bytes[0] === 0) {
        warnings.push('这一帧不像完整逻辑指令（首字节应为地址、末字节应为固定校验 6B）；原始路径不做结构检查，字节原样下发。');
      }
      return {
        summary: `原始逻辑指令 · ${action.bytes.length} 字节：${hex}`,
        detail: '绕过功能码白名单原样下发：无运动监督、不推断完成、步骤结束后不会自动停止。',
        warnings,
        raw: true,
      };
    }
    case 'can': {
      const hex = action.data.map((byte) => byte.toString(16).toUpperCase().padStart(2, '0')).join(' ');
      const id = action.canId.toString(16).toUpperCase().padStart(action.extended ? 8 : 3, '0');
      return {
        summary: `原始 CAN ${action.extended ? '扩展帧' : '标准帧'} · ID 0x${id} · ${action.data.length} 字节：${hex || '（无数据）'}`,
        detail: '真实总线数据帧：不重写、不追加 6B、不重试、不推断完成；相邻帧之间至少 2 ms。',
        warnings,
        raw: true,
      };
    }
    default:
      return { summary: `${action.verb}（未识别）`, warnings };
  }
}

/**
 * Parse + preview in one call: what the editor needs to render errors, the
 * source-line preview table and the counts, plus the framing-level verdict.
 */
export function validateProgram(text, { distances = null } = {}) {
  const parsed = parseProgram(text);
  const errors = [...parsed.errors];
  const warnings = [];
  const bytes = utf8Length(String(text ?? ''));

  if (bytes > QUEUE_LIMITS.maxTextBytes) {
    errors.push({ line: 0, message: `程序文本 ${bytes} 字节，超过板端上限 ${QUEUE_LIMITS.maxTextBytes} 字节` });
  }
  if (parsed.actions.length > QUEUE_LIMITS.maxActions) {
    errors.push({ line: 0, message: `程序有 ${parsed.actions.length} 个动作，超过板端上限 ${QUEUE_LIMITS.maxActions} 个` });
  }
  if (parsed.actions.length === 0 && errors.length === 0) {
    errors.push({ line: 0, message: '程序里没有任何动作：板端会以“程序为空”拒绝。' });
  }

  const preview = parsed.actions.map((action) => {
    const entry = previewAction(action, { distances });
    (entry.warnings ?? []).forEach((message) => warnings.push({ line: action.line, message }));
    return {
      line: action.line,
      verb: action.verb,
      label: getVerbDefinition(action.verb)?.label ?? action.verb,
      raw: entry.raw === true,
      summary: entry.summary,
      detail: entry.detail ?? null,
    };
  });

  const byVerb = {};
  parsed.actions.forEach((action) => { byVerb[action.verb] = (byVerb[action.verb] ?? 0) + 1; });

  // Only a confirmed "no distance saved for this ID" blocks submission: the
  // board cannot convert mm without it. An unread profile stays a warning,
  // because a failed read must never be turned into an assumed value.
  parsed.actions.forEach((action) => {
    if (action.verb !== 'move' || action.unit !== 'mm') return;
    const angle = actionAngleDegrees(action, distances);
    if (!angle.ok && angle.reason === 'distance_missing') {
      errors.push({ line: action.line, message: `电机 ${action.id} 的 mm/rev 旋转距离为 0（未配置）：mm 行程无法换算，请先在右侧保存该地址的旋转距离，或改用 deg/rev。` });
    }
  });

  return {
    ok: errors.length === 0,
    actions: parsed.actions,
    errors,
    warnings,
    preview,
    stats: { bytes, actions: parsed.actions.length, lines: parsed.lineCount, byVerb, usedIds: [...new Set(parsed.actions.map((action) => action.id).filter((id) => Number.isInteger(id)))].sort((a, b) => a - b) },
  };
}

// ---------------------------------------------------------------------------
// Action builder
// ---------------------------------------------------------------------------

/** Editable defaults for the insertion form of one verb. */
export function builderDefaults(verb) {
  const definition = getVerbDefinition(verb);
  if (!definition) return {};
  const values = {};
  definition.args.forEach((arg) => { values[arg.key] = arg.default ?? ''; });
  return values;
}

/**
 * Build the source line for the builder. The result is parsed again, so a line
 * that would not survive the parser is reported instead of inserted.
 */
export function buildActionLine(verb, values) {
  const definition = getVerbDefinition(verb);
  if (!definition) return { ok: false, error: `未知指令「${verb}」` };
  const parts = [verb];
  for (const arg of definition.args) {
    const raw = String(values?.[arg.key] ?? '').trim();
    if (!raw) {
      if (arg.default !== undefined) parts.push(arg.default);
      else return { ok: false, error: `「${arg.label}」不能为空` };
      continue;
    }
    parts.push(raw);
  }
  const line = parts.join(' ');
  const parsed = parseProgram(line);
  if (parsed.errors.length > 0) return { ok: false, error: parsed.errors[0].message, line };
  const [action] = parsed.actions;
  return { ok: true, line, action, preview: previewAction(action, { distances: null }) };
}

/** Short summary of one parsed action, used for the builder's live hint. */
export const describeAction = (action, options) => previewAction(action, options).summary;

/**
 * Which motors the program has switched on by the time it reaches the end.
 *
 * The enable state is followed in order, not accumulated: `enable 1` followed by
 * `disable 1` leaves motor 1 *off*, so the builder must not treat an earlier
 * enable line as still valid. `stop` keeps the state (the board preserves
 * enable), while a raw `hex` / `can` step can carry any frame at all — including
 * a disable — so it invalidates everything the page thought it knew.
 */
export function knownEnabledIds(actions) {
  const enabled = new Set();
  for (const action of actions) {
    if (action.verb === 'enable') enabled.add(action.id);
    else if (action.verb === 'disable') enabled.delete(action.id);
    else if (action.verb === 'hex' || action.verb === 'can') enabled.clear();
  }
  return enabled;
}

/**
 * Sample program: six lines, the three motors the user actually drives, each
 * enabled explicitly. No stop/disable padding — a timed torque or velocity step
 * stops itself, and the normal stop keeps enable on purpose; `disable` stays
 * available for when the motor really should be switched off (see the help).
 */
export const SAMPLE_PROGRAM = [
  '# 编排队列示例：1 号相对运动 → 2 号回零 → 3 号限速力矩',
  '# 每行一个动作；# 之后是注释；指令与单位不分大小写；可选参数只能写在最后。',
  'enable 1            # 等待真实 F3 应答；程序不会替电机隐式使能',
  'move 1 90           # 单位默认 deg，RPM 30、加减速 60、电流 800 mA',
  'enable 2',
  'home 2              # 模式默认 0（单圈就近），等待受监督的 9A 结果',
  'enable 3',
  'torque 3 800 1500   # 800 mA 持续 1500 ms，到时自动停止并等待静止',
].join('\n');
