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
  unverifiedRepeatMax: 0x7fffffff, // ESP32 long request field before uint32 storage
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
  minAccelRpmS: 0,
  maxAccelRpmS: 65535,
  minCurrentMa: 0,
  maxCurrentMa: 5000,
  // Direct-send mode keeps only the fields the board can encode. Queue moves
  // store a signed tenths count even though CD itself has unsigned magnitude.
  unverifiedMaxMoveTenths: 0x7fffffff,
  unverifiedMaxSpeedRpm: 0xffff / 10,
  unverifiedMaxCurrentMa: 0xffff,
  unverifiedMaxDurationMs: 1000000000,
};

const ID_SPAN = `${QUEUE_LIMITS.addressMin}..${QUEUE_LIMITS.addressMax}`;

const NUMBER_RE = /^-?\d+(?:\.\d+)?$/;
const INTEGER_RE = /^-?\d+$/;
const ADDRESS_RE = /^\d+$/;
const HEX_BYTE_RE = /^[0-9a-fA-F]{2}$/;
const CAN_ID_RE = /^(?:0x)?([0-9a-fA-F]{1,8})$/;
const exactTenths = value => Number.isSafeInteger(Math.abs(value) * 10);

/**
 * Every verb the board accepts, with the short form and the defaults that the
 * help list has to show verbatim. `args` doubles as the action builder's field
 * list, in wire order: the optional ones are trailing only.
 */
export const QUEUE_VERBS = [
  {
    verb: 'sync', label: '同步组边界', usage: 'sync begin | sync begin trigger | sync end', shortForm: 'sync begin',
    defaults: '组内 2–8 个不同地址的相对 move',
    note: '共同轨迹、一次触发、全部到位后继续；trigger 适用于高速，只验证共同触发与最终到位，不保证运行中 2% 进度。异常请求成员停止并保留使能。必须先配置反馈预算与同步容差、完成缓存隔离台架确认。',
    args: [{ key: 'boundary', label: '边界 begin、begin trigger 或 end', kind: 'raw', default: 'begin' }],
  },
  {
    verb: 'helix', label: '螺旋动作',
    usage: 'helix ROTARY_ID LINEAR_ID TURNS LEAD RATIO ROTARY_DIR LINEAR_DIR RPM ACCEL DECEL CURRENT TOL_MM',
    shortForm: '所有参数必填', defaults: '无默认机械参数；直线轴 mm/rev 使用板端已保存值',
    note: '轴向行程＝导程×瓶盖圈数；板端统一换算为双轴同步组。方向填写 1 或 -1。不会识别螺纹脱离，抬升另写一行。',
    args: [
      {key:'id',label:'旋转电机地址',kind:'address'}, {key:'linearId',label:'直线电机地址',kind:'address'},
      {key:'turns',label:'瓶盖圈数',kind:'number'}, {key:'lead',label:'导程',unit:'mm/瓶盖圈',kind:'number'},
      {key:'ratio',label:'传动比',unit:'电机圈/瓶盖圈',kind:'number'},
      {key:'rotaryDir',label:'旋转方向',unit:'1 或 -1',kind:'number'}, {key:'linearDir',label:'直线方向',unit:'1 或 -1',kind:'number'},
      {key:'rpm',label:'各轴转速上限',unit:'RPM',kind:'number'},
      {key:'accel',label:'各轴加速度上限',unit:'RPM/s',kind:'number'}, {key:'decel',label:'各轴减速度上限',unit:'RPM/s',kind:'number'},
      {key:'current',label:'各轴电流上限',unit:'mA',kind:'number'}, {key:'tolerance',label:'轴向允许偏差',unit:'mm',kind:'number'},
    ],
  },
  {
    verb: 'enable',
    label: '使能',
    usage: 'enable ID',
    shortForm: 'enable 1',
    defaults: '地址必填，没有隐式使能',
    note: '只发送 F3 使能帧，不等应答。程序不会替任何电机隐式使能：需要就先自己写一行。',
    args: [{ key: 'id', label: '电机地址', kind: 'address', default: '1' }],
  },
  {
    verb: 'disable',
    label: '失能',
    usage: 'disable ID',
    shortForm: 'disable 1',
    defaults: '地址必填',
    note: '只发送 F3 关闭使能帧，不等应答、也不等待静止确认。示例程序故意不写 disable：需要断电或人工干预时再加这一行。',
    args: [{ key: 'id', label: '电机地址', kind: 'address', default: '1' }],
  },
  {
    verb: 'move',
    label: '相对运动',
    usage: 'move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]] [await]',
    shortForm: 'move 1 90',
    defaults: '单位 deg · RPM 30 · 加减速 60 · 电流 800 mA',
    note: 'VALUE 带符号表示方向；rev = 360°；mm 用该地址已保存的 mm/rev 换算。发送 CD 相对模式 2；默认发送后继续，末尾加 await 才等待到位。',
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
    usage: 'home ID [MODE] [await]',
    shortForm: 'home 2',
    defaults: '模式 0（单圈就近）',
    note: '发送 9A 回零触发；默认发送后继续，末尾加 await 才等待回零完成；12/22 表示驱动报告无需运动。异常只报告，不自动停机或失能。',
    args: [
      { key: 'id', label: '电机地址', kind: 'address', default: '2' },
      { key: 'mode', label: '回零模式', kind: 'number', unit: '0..5', default: '0' },
    ],
  },
  {
    verb: 'torque',
    label: '限速力矩',
    usage: 'torque ID SIGNED_MA [DURATION_MS [MAX_RPM [RAMP_MA_S]]]',
    shortForm: 'torque 3 -300 1500',
    defaults: '限速 30 RPM · 斜率 1000 mA/s',
    note: 'C5 限速力矩：SIGNED_MA 带符号决定方向。写持续时间＝到时补一条 FE 停止后立刻继续（不等静止）；不写持续时间＝只发送、继续下一行，不会自动停止。力矩以 mA 下发，界面不使用 Nm。',
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
    usage: 'velocity ID SIGNED_RPM [DURATION_MS [ACCEL [CURRENT]]]',
    shortForm: 'velocity 1 60 2000',
    defaults: '加速度 60 RPM/s · 电流 800 mA',
    note: 'C6 限流速度：SIGNED_RPM 带符号决定方向。写持续时间＝到时补一条 FE 停止后立刻继续（不等静止）；不写持续时间＝只发送、继续下一行，不会自动停止。',
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
    note: '只发送停止帧，不等待静止反馈；保留使能状态（要真正断电请再写一行 disable）。',
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

function readAddress(token, fail, unverified = false) {
  if (!ADDRESS_RE.test(token)) {
    fail(`电机地址必须是十进制整数（${unverified ? '0..255' : ID_SPAN}）：${token}`);
    return null;
  }
  return readNumber(token, unverified ? {...addressSpec,min:0} : addressSpec, fail);
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

function parseHexLine(tokens, fail, unverified = false) {
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
  if (unverified && bytes.at(-1) !== 0x6b) {
    fail('不校验模式的原始逻辑指令末字节必须是 6B；要发送任意 CAN 数据请用 can');
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

function parseMove(tokens, fail, unverified = false) {
  const [idToken, valueToken, ...rest] = tokens;
  if (idToken === undefined) {
    fail('缺少电机地址（用法：move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]]）');
    return null;
  }
  if (valueToken === undefined) {
    fail('缺少行程 VALUE（用法：move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]]）');
    return null;
  }
  const id = readAddress(idToken, fail, unverified);
  const value = readNumber(valueToken, { label: '行程', min: unverified ? 0 : QUEUE_LIMITS.minAngleDeg, signed: true, absMin: unverified ? 0 : QUEUE_LIMITS.minAngleDeg }, fail);
  if (id == null || value == null) return null;
  let unit = 'deg';
  if (rest.length && QUEUE_UNITS.includes(rest[0].toLowerCase())) unit = rest.shift().toLowerCase();
  if (unverified && unit === 'deg' && !exactTenths(value)) {
    fail('直通 move 的 deg 行程必须精确为 0.1° 的整数倍；rev/mm 仍由板端换算');
    return null;
  }
  const optional = [unverified ? {...SPEED,min:0,max:QUEUE_LIMITS.unverifiedMaxSpeedRpm} : SPEED,
    ACCEL, ACCEL, unverified ? {...CURRENT,max:QUEUE_LIMITS.unverifiedMaxCurrentMa} : CURRENT];
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
  if (unverified && !exactTenths(values.rpm)) {
    fail('直通 move 速度必须精确为 0.1 RPM 的整数倍');
    return null;
  }
  return { id, value, unit, ...values };
}

function parseTorque(tokens, fail, unverified = false) {
  // torque ID SIGNED_MA [DURATION_MS [MAX_RPM [RAMP_MA_S]]]
  // Without a duration the frame is sent and the next line follows immediately:
  // no timer and no implicit stop.
  const [idToken, currentToken, durationToken, ...rest] = tokens;
  if (idToken === undefined || currentToken === undefined) {
    fail('用法：torque ID SIGNED_MA [DURATION_MS [MAX_RPM [RAMP_MA_S]]]');
    return null;
  }
  const id = readAddress(idToken, fail, unverified);
  const currentMa = readNumber(currentToken, { label: '力矩电流', integer: true, signed: true, absMin: 0, absMax: unverified ? QUEUE_LIMITS.unverifiedMaxCurrentMa : QUEUE_LIMITS.maxCurrentMa, unit: 'mA' }, fail);
  const durationMs = durationToken === undefined ? 0 : readNumber(durationToken, unverified ? {...DURATION,min:0,max:QUEUE_LIMITS.unverifiedMaxDurationMs} : DURATION, fail);
  if (id == null || currentMa == null || durationMs == null) return null;
  if (rest.length > 2) {
    fail(`限速力矩最多 2 个可选参数（MAX_RPM RAMP_MA_S），多出 ${rest.length - 2} 个`);
    return null;
  }
  const maxRpm = rest.length > 0
    ? readNumber(rest[0], { ...SPEED, min: 0, max:unverified ? QUEUE_LIMITS.unverifiedMaxSpeedRpm : SPEED.max, label: '限速' }, fail)
    : 30;
  const rampMaS = rest.length > 1
    ? readNumber(rest[1], { label: '电流斜率', integer: true, min: 0, max: QUEUE_LIMITS.maxAccelRpmS, unit: 'mA/s' }, fail)
    : 1000;
  if (maxRpm == null || rampMaS == null) return null;
  if (unverified && !exactTenths(maxRpm)) {
    fail('直通 torque 限速必须精确为 0.1 RPM 的整数倍');
    return null;
  }
  return { id, currentMa, durationMs, maxRpm, rampMaS };
}

function parseVelocity(tokens, fail, unverified = false) {
  // velocity ID SIGNED_RPM [DURATION_MS [ACCEL [CURRENT]]]
  const [idToken, rpmToken, durationToken, ...rest] = tokens;
  if (idToken === undefined || rpmToken === undefined) {
    fail('用法：velocity ID SIGNED_RPM [DURATION_MS [ACCEL [CURRENT]]]');
    return null;
  }
  const id = readAddress(idToken, fail, unverified);
  const rpm = readNumber(rpmToken, { ...SPEED, label: '转速', signed: true, absMin: unverified ? 0 : QUEUE_LIMITS.minSpeedRpm, absMax: unverified ? QUEUE_LIMITS.unverifiedMaxSpeedRpm : QUEUE_LIMITS.maxSpeedRpm }, fail);
  const durationMs = durationToken === undefined ? 0 : readNumber(durationToken, unverified ? {...DURATION,min:0,max:QUEUE_LIMITS.unverifiedMaxDurationMs} : DURATION, fail);
  if (id == null || rpm == null || durationMs == null) return null;
  if (rest.length > 2) {
    fail(`速度指令最多 2 个可选参数（ACCEL CURRENT），多出 ${rest.length - 2} 个`);
    return null;
  }
  const accel = rest.length > 0 ? readNumber(rest[0], { ...ACCEL, label: '加速度' }, fail) : 60;
  const current = rest.length > 1 ? readNumber(rest[1], unverified ? {...CURRENT,max:QUEUE_LIMITS.unverifiedMaxCurrentMa} : CURRENT, fail) : 800;
  if (accel == null || current == null) return null;
  if (unverified && !exactTenths(rpm)) {
    fail('直通 velocity 速度必须精确为 0.1 RPM 的整数倍');
    return null;
  }
  return { id, rpm, durationMs, accel, current };
}

/** Parse one already-tokenised action line. Returns null when `fail` was called. */
function parseTokens(verb, tokens, fail, unverified = false) {
  switch (verb) {
    case 'sync':
      if(tokens.length===2 && tokens[0].toLowerCase()==='begin' && tokens[1].toLowerCase()==='trigger') {
        return {boundary:'begin',triggerOnly:true};
      }
      if(tokens.length!==1 || !['begin','end'].includes(tokens[0].toLowerCase())) {
        fail('同步边界只能是 sync begin、sync begin trigger 或 sync end');return null;
      }
      return {boundary:tokens[0].toLowerCase()};
    case 'helix': {
      if(tokens.length!==12) {fail('helix 的 12 个参数必须全部明确填写');return null;}
      const id=readAddress(tokens[0],fail,unverified),linearId=readAddress(tokens[1],fail,unverified);
      if(id==null || linearId==null) return null;
      if(id===0 || linearId===0) {fail('螺旋动作需要可单独寻址的电机（1..255）');return null;}
      if(id===linearId) {fail('螺旋动作需要两个不同电机地址');return null;}
      if(tokens.slice(2).some(t=>!NUMBER_RE.test(t))) {fail('helix 只接受有限十进制数值');return null;}
      const [turns,lead,ratio,rotaryDir,linearDir,rpm,accel,decel,current,tolerance]=tokens.slice(2).map(Number);
      if(![turns,lead,ratio,rotaryDir,linearDir,rpm,accel,decel,current,tolerance].every(Number.isFinite) ||
         turns===0 || lead<=0 || ratio<=0 || ![-1,1].includes(rotaryDir) || ![-1,1].includes(linearDir) ||
         rpm<(unverified ? 0 : 0.1) || rpm>(unverified ? QUEUE_LIMITS.unverifiedMaxSpeedRpm : 3000) || ![accel,decel].every(v=>Number.isInteger(v)&&v>=(unverified ? 0 : 1)&&v<=65535) ||
         !Number.isInteger(current) || current<0 || current>(unverified ? QUEUE_LIMITS.unverifiedMaxCurrentMa : 5000) || (!unverified && tolerance<=0)) {
        fail('helix 几何、方向、运动上限或容差不合法');return null;
      }
      return {id,linearId,turns,lead,ratio,rotaryDir,linearDir,rpm,accel,decel,current,tolerance};
    }
    case 'enable':
    case 'disable':
    case 'stop': {
      if (tokens.length !== 1) {
        fail(`用法：${verb} ID（只接受一个电机地址）`);
        return null;
      }
      const id = readAddress(tokens[0], fail, unverified);
      return id == null ? null : { id };
    }
    case 'wait': {
      if (tokens.length !== 1) {
        fail('用法：wait MS（整数 0..3600000）');
        return null;
      }
      const ms = readNumber(tokens[0], unverified ? {...WAIT_MS,max:QUEUE_LIMITS.unverifiedMaxDurationMs} : WAIT_MS, fail);
      return ms == null ? null : { ms };
    }
    case 'home': {
      if (tokens.length < 1 || tokens.length > 2) {
        fail('用法：home ID [MODE]（模式 0..5，默认 0）');
        return null;
      }
      const id = readAddress(tokens[0], fail, unverified);
      const mode = tokens.length === 2 ? readNumber(tokens[1], unverified ? {...HOME_MODE,max:255} : HOME_MODE, fail) : 0;
      if (id == null || mode == null) return null;
      return { id, mode };
    }
    case 'move': return parseMove(tokens, fail, unverified);
    case 'torque': return parseTorque(tokens, fail, unverified);
    case 'velocity': return parseVelocity(tokens, fail, unverified);
    case 'hex': {
      const parsed = parseHexLine(tokens, fail, unverified);
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
export function parseProgram(text, { unverified = false } = {}) {
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
    const awaitIndex = tokens.findIndex((token, i) => i > 0 && token.toLowerCase() === 'await');
    const awaitCompletion = awaitIndex !== -1;
    if (awaitCompletion) {
      if (!['move', 'home'].includes(verb) || awaitIndex !== tokens.length - 1) {
        fail('await 只能放在 move/home 指令末尾，且只能出现一次');
        return;
      }
      tokens.pop();
    }
    const body = parseTokens(verb, tokens.slice(1), fail, unverified);
    if (body && ['move', 'home'].includes(verb)) body.awaitCompletion = awaitCompletion;
    if (body) actions.push({ line, verb, ...body });
  });
  return { actions, errors, lineCount: lines.length };
}

/** Repeat count from the form: 1..1000, integers only. */
export function checkRepeat(raw, { unverified = false } = {}) {
  const text = String(raw ?? '').trim();
  const max = unverified ? QUEUE_LIMITS.unverifiedRepeatMax : QUEUE_LIMITS.repeatMax;
  if (!INTEGER_RE.test(text)) return { ok: false, error: `重复次数只能是 1..${max} 的整数` };
  const value = Number(text);
  if (!Number.isSafeInteger(value) || value < QUEUE_LIMITS.repeatMin || value > max) {
    return { ok: false, error: `重复次数需在 ${QUEUE_LIMITS.repeatMin}..${max} 之间` };
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
export function previewAction(action, { distances = null, unverified = false } = {}) {
  const warnings = [];
  switch (action.verb) {
    case 'sync': return {summary:unverified
      ? action.boundary==='begin' ? '同步组开始：按成员发送缓存帧，再提交单次 FF；不判定到位' : '同步组结束：缓存与触发提交结束'
      : action.boundary==='begin'
      ? action.triggerOnly?'高速同步组开始：共同触发，核对最终到位；不验证运行中 2% 进度':'同步组开始：先检查，再缓存与单次触发'
      :'同步组结束：全部成员到位后继续',warnings};
    case 'helix': return {
      summary:`螺旋：旋转轴 ${action.id} / 直线轴 ${action.linearId} · ${action.turns} 瓶盖圈 · 轴向 ${num(action.turns*action.lead)} mm`,
      detail:`方向 ${action.rotaryDir}/${action.linearDir}；传动比 ${action.ratio}；轴向容差 ${action.tolerance} mm；${unverified ? '板端仅换算并提交可编码帧，不判断机械配合或到位。' : '板端换算与预算校验。未自动判断脱扣。'}`,
      warnings:distanceLookup(distances,action.linearId).state==='known'?[]:['直线轴 mm/rev 尚未确认，不能预判换算结果。'],
    };
    case 'enable':
      return { summary: `电机 ${action.id}：发送使能 F3（不等应答）`, warnings };
    case 'disable':
      return { summary: `电机 ${action.id}：发送关闭使能 F3（不等应答）`, warnings };
    case 'stop':
      return { summary: `电机 ${action.id}：发送停止 FE（保留使能状态）`, warnings };
    case 'wait':
      return { summary: `等待 ${action.ms} ms（非阻塞，只推迟后续动作）`, warnings };
    case 'home':
      return {
        summary: `电机 ${action.id}：发送回零触发 9A · 模式 ${action.mode} ${homeModeLabel(action.mode)}（${action.awaitCompletion && !unverified ? '等待完成' : '发送后继续，不判定完成'}）`,
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
        if (counts < 1 && !unverified) {
          warnings.push(`行程换算后不足 1 个 0.1° 计数（${counts}），板端会以“取整为零”拒绝。`);
        }
      } else if (angle.reason === 'distance_missing') {
        warnings.push(`电机 ${action.id} 尚未保存 mm/rev 旋转距离：mm 行程无法换算角度，板端会拒绝这一步。`);
        detail = `旋转距离未知 · ${detail}`;
      } else {
        warnings.push(`尚未从板端读取电机 ${action.id} 的 mm/rev 旋转距离，本页不推测换算结果。`);
        detail = `旋转距离未读取 · ${detail}`;
      }
      return { summary: `${head}（${action.awaitCompletion && !unverified ? '等待到位' : '发送后继续，不判定到位'}）`, detail, warnings, counts, deg: angle.ok ? angle.deg : null };
    }
    case 'torque':
      return {
        summary: action.durationMs
          ? `电机 ${action.id}：限速力矩 ${signed(action.currentMa)} mA · ${action.durationMs} ms · 限速 ${num(action.maxRpm)} RPM · 斜率 ${action.rampMaS} mA/s`
          : `电机 ${action.id}：限速力矩 ${signed(action.currentMa)} mA · 限速 ${num(action.maxRpm)} RPM · 斜率 ${action.rampMaS} mA/s`,
        detail: action.durationMs
          ? '到时补一条 FE 停止后立刻继续（不等静止）；力矩以 mA 下发（不使用 Nm）'
          : '只发送、继续下一行，不会自动停止；力矩以 mA 下发（不使用 Nm）',
        warnings,
      };
    case 'velocity':
      return {
        summary: action.durationMs
          ? `电机 ${action.id}：速度 ${signed(action.rpm)} RPM · ${action.durationMs} ms`
          : `电机 ${action.id}：速度 ${signed(action.rpm)} RPM`,
        detail: action.durationMs
          ? `加速度 ${num(action.accel)} RPM/s · 电流上限 ${action.current} mA · 到时补一条 FE 停止后立刻继续`
          : `加速度 ${num(action.accel)} RPM/s · 电流上限 ${action.current} mA · 只发送，不会自动停止`,
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
export function validateProgram(text, { distances = null, unverified = false } = {}) {
  const parsed = parseProgram(text,{unverified});
  const errors = [...parsed.errors];
  const warnings = [];
  const bytes = utf8Length(String(text ?? ''));

  if (bytes > QUEUE_LIMITS.maxTextBytes) {
    errors.push({ line: 0, message: `程序文本 ${bytes} 字节，超过板端上限 ${QUEUE_LIMITS.maxTextBytes} 字节` });
  }
  const expandedCount=parsed.actions.reduce((n,a)=>n+(a.verb==='helix'?4:1),0);
  if (expandedCount > QUEUE_LIMITS.maxActions) {
    errors.push({ line: 0, message: `程序展开后有 ${expandedCount} 个动作，超过板端上限 ${QUEUE_LIMITS.maxActions} 个` });
  }
  let group=null;
  for(const a of parsed.actions) {
    const fail=message=>errors.push({line:a.line,message});
    if(a.verb==='sync') {
      if(a.boundary==='begin') {
        if(group) fail('同步组禁止嵌套');else group={line:a.line,ids:new Set(),count:0};
      } else if(!group) fail('sync end 缺少对应的 begin');
      else {if(group.count<2 || group.count>8) fail('同步组需要 2–8 个成员');group=null;}
    } else if(group) {
      if(a.verb!=='move' || a.awaitCompletion) fail('同步组内只允许相对 move，不写 await');
      else {
        if(a.id===0) fail('同步组成员需使用独立地址 1..255');
        if(group.ids.has(a.id)) fail('同步组电机地址不能重复');
        group.ids.add(a.id);group.count++;
        if(!unverified && (a.accel===0 || a.decel===0)) fail('同步组加减速度必须大于零');
      }
    }
    if(a.verb==='helix' && distanceLookup(distances,a.linearId).state==='none') fail(`直线电机 ${a.linearId} 未配置 mm/rev`);
  }
  if(group) errors.push({line:group.line,message:'sync begin 缺少对应的 end'});
  if (parsed.actions.length === 0 && errors.length === 0) {
    errors.push({ line: 0, message: '程序里没有任何动作：板端会以“程序为空”拒绝。' });
  }

  const preview = parsed.actions.map((action) => {
    const entry = previewAction(action, { distances, unverified });
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

  if (unverified) parsed.actions.forEach((action) => {
    if (action.verb !== 'move') return;
    const angle = actionAngleDegrees(action, distances);
    if (angle.ok && (!Number.isFinite(angle.deg) || Math.abs(angle.deg * 10) > QUEUE_LIMITS.unverifiedMaxMoveTenths)) {
      errors.push({line:action.line,message:'换算后的行程超出队列有符号 32 位的 0.1° 字段'});
    }
  });

  return {
    ok: errors.length === 0,
    actions: parsed.actions,
    errors,
    warnings,
    preview,
    stats: { bytes, actions: expandedCount, lines: parsed.lineCount, byVerb, usedIds: [...new Set(parsed.actions.flatMap(a=>[a.id,a.linearId]).filter(Number.isInteger))].sort((a,b)=>a-b) },
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
export function buildActionLine(verb, values, { unverified = false } = {}) {
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
  const parsed = parseProgram(line,{unverified});
  if (parsed.errors.length > 0) return { ok: false, error: parsed.errors[0].message, line };
  const [action] = parsed.actions;
  return { ok: true, line, action, preview: previewAction(action, { distances: null, unverified }) };
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
 * Sample program: the three motors the user actually drives, each enabled
 * explicitly (the board never enables anything on its own). Frames go out in
 * order and nothing is awaited: `wait` lines are the only waiting, and a timed
 * torque step adds its own FE after the written duration.
 */
export const SAMPLE_PROGRAM = [
  '# 编排队列示例：1 号相对运动 → 2 号回零 → 3 号限速力矩',
  '# 默认发送后继续；末尾 await 等本次动作完成，wait 用于固定延时。',
  'enable 1            # 只发送 F3；程序不会替电机隐式使能',
  'move 1 90 await     # 单位默认 deg，RPM 30、加减速 60、电流 800 mA',
  'wait 1500           # 到位后额外停留 1.5 秒',
  'enable 2',
  'home 2 await        # 等待回零完成后继续',
  'enable 3',
  'torque 3 800 1500   # 保持 800 mA 1500 ms，到时补一条 FE 停止',
  'torque 3 0          # 不带持续时间：只发送，不会自动停止',
].join('\n');
