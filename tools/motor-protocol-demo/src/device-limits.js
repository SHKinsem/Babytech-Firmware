// Board motion policy: the controller stores an editable limit set in NVS and
// exposes it through /api/limits. Two different things are deliberately kept
// apart here:
//
//   * the implementation/wire ceilings below — fixed protocol or firmware
//     bounds that can never move (a 0.1 RPM wire field, a uint16 accel, the
//     manual's 3000 RPM / 5000 mA and this implementation's timer/travel ceilings);
//   * the editable limits — board policy the user can change on the 调试限制
//     tab and save to NVS.
//
// Nothing in this module talks to the board. It only validates payloads and
// converts a confirmed limit set into the shapes the planner and the gate use.
//
// Pure module: no React, no timers, no network.

import { MANUAL_LIMITS, defaultValues } from './protocol.js';

// Initial values documented by the firmware contract. They are DEFAULTS only:
// this page never treats them as loaded limits until /api/limits answers.
export const DEFAULT_LIMITS = {
  maxSpeedRpm: 120,
  maxAccelRpmS: 240,
  maxCurrentMa: 5000,
  maxAngleDeg: 3600,
  maxMoveSeconds: 60,
  experimentSeconds: 5,
};

// Field order is the wire/nvs order and the form order.
export const LIMIT_FIELDS = [
  {
    key: 'maxSpeedRpm',
    label: '速度上限',
    unit: 'RPM',
    integer: false,
    min: 0.1,
    max: 3000,
    hint: '运动指令允许的最高转速，按 0.1 RPM 下发。手册标称 X 固件上限 3000 RPM（原文该处十六进制写作 7E30，30000 应为 7530，属手册笔误，本页按 3000 RPM 校验）。',
  },
  {
    key: 'maxAccelRpmS',
    label: '加速度上限',
    unit: 'RPM/s',
    integer: true,
    min: 1,
    max: 65535,
    hint: '速度指令的加速度与位置指令的加/减速度上限，整数 RPM/s（线上不乘 10）。',
  },
  {
    key: 'maxCurrentMa',
    label: '电流上限',
    unit: 'mA',
    integer: true,
    min: 100,
    max: 5000,
    hint: '约束 F5/C5 的力矩电流及 C6/CD、常规试动中的电流字段。F6 不携带电流参数，其电流由驱动器配置决定；需要指定限流请选 C6。手册电流上限为 5000 mA。',
  },
  {
    key: 'maxAngleDeg',
    label: '单次行程上限',
    unit: '°',
    integer: false,
    min: 0.1,
    max: 360000,
    hint: '单条位置指令允许的最大行程幅值，按 0.1° 计数下发；方向仍由方向字段决定。',
  },
  {
    key: 'maxMoveSeconds',
    label: '单次运动时长上限',
    unit: 's',
    integer: true,
    min: 1,
    max: 3600,
    hint: '网页规划运动时允许的最长预估时长；超出直接报错，不做静默截断。',
  },
  {
    key: 'experimentSeconds',
    label: '试验运行时长',
    unit: 's',
    integer: true,
    min: 0,
    max: 3600,
    hint: '速度／力矩试验的自动停止时间。0 表示持续运行、需要手动停止（反馈超时与故障保护仍然独立生效）。',
  },
];

export const LIMIT_KEYS = LIMIT_FIELDS.map((field) => field.key);

const FIELD_BY_KEY = new Map(LIMIT_FIELDS.map((field) => [field.key, field]));

const round = (value, decimals) => Number(value.toFixed(decimals));

/**
 * Validate one editable field. Accepts user text or a payload number; rejects
 * anything outside the implementation ceiling instead of clamping it.
 */
export function checkLimitField(key, raw) {
  const field = FIELD_BY_KEY.get(key);
  if (!field) return { ok: false, error: '未知限制字段' };
  const text = typeof raw === 'string' ? raw.trim() : raw;
  if (text === '' || text == null) return { ok: false, error: `${field.label}不能为空` };
  const value = Number(text);
  if (!Number.isFinite(value)) return { ok: false, error: `${field.label}必须是有限数字` };
  if (field.integer && !Number.isInteger(value)) {
    return { ok: false, error: `${field.label}必须是整数 ${field.unit}` };
  }
  if (!field.integer && Math.abs(Math.round(value * 10) - value * 10) > 1e-6) {
    return { ok: false, error: `${field.label}最多保留一位小数` };
  }
  if (value < field.min || value > field.max) {
    return { ok: false, error: `${field.label}需在 ${field.min}..${field.max} ${field.unit}（实现上限，不做截断）` };
  }
  return { ok: true, value: round(value, 1) };
}

/**
 * Parse a /api/limits payload. Every one of the six fields must be present and
 * inside the ceilings; a partial or malformed payload is rejected so the caller
 * can keep motion disabled instead of inventing defaults.
 */
export function readLimitsPayload(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) {
    return { ok: false, error: '板端返回的限制不是有效对象' };
  }
  const limits = {};
  const errors = {};
  for (const field of LIMIT_FIELDS) {
    const check = checkLimitField(field.key, payload[field.key]);
    if (check.ok) limits[field.key] = check.value;
    else errors[field.key] = check.error;
  }
  if (Object.keys(errors).length > 0) return { ok: false, error: '板端限制字段缺失或超出实现上限', errors };
  return { ok: true, limits, errors: {} };
}

/** Validate a whole draft form. Returns the payload to POST when every field passes. */
export function checkLimitsDraft(values) {
  const limits = {};
  const errors = {};
  for (const field of LIMIT_FIELDS) {
    const check = checkLimitField(field.key, values?.[field.key]);
    if (check.ok) limits[field.key] = check.value;
    else errors[field.key] = check.error;
  }
  return { ok: Object.keys(errors).length === 0, limits, errors };
}

/** Editable strings for the form (keeps "30.0" style values intact). */
export function limitsToDraft(limits) {
  const draft = {};
  for (const field of LIMIT_FIELDS) {
    const value = limits?.[field.key];
    draft[field.key] = value == null ? '' : String(value);
  }
  return draft;
}

export const limitsEqual = (a, b) => LIMIT_KEYS.every((key) => Number(a?.[key]) === Number(b?.[key]));

/**
 * Confirmed limits in the shape MotionCore/the planner expects. Minimums stay
 * at the firmware values: only the ceiling of each range is board policy.
 */
export function limitsToManualLimits(limits) {
  if (!limits) return MANUAL_LIMITS;
  return {
    minAbsAngleDeg: MANUAL_LIMITS.minAbsAngleDeg,
    maxAbsAngleDeg: limits.maxAngleDeg,
    minSpeedRpm: MANUAL_LIMITS.minSpeedRpm,
    maxSpeedRpm: limits.maxSpeedRpm,
    minAccelRpmS: MANUAL_LIMITS.minAccelRpmS,
    maxAccelRpmS: limits.maxAccelRpmS,
    minCurrentMa: MANUAL_LIMITS.minCurrentMa,
    maxCurrentMa: limits.maxCurrentMa,
    maxExpectedDurationMs: limits.maxMoveSeconds * 1000,
  };
}

/** Human wording for the configured experiment window (0 = no timer). */
export function experimentWindowText(limits) {
  const seconds = Number(limits?.experimentSeconds);
  if (!Number.isFinite(seconds)) return null;
  return seconds === 0 ? '持续运行，手动停止' : `板端最多运行 ${seconds} 秒`;
}

/**
 * Catalog defaults adjusted to the confirmed board limits. Only used when a
 * command is (re)selected: an edited value is never rewritten behind the user.
 */
export function deviceDefaults(item, variantKey, limits) {
  const values = defaultValues(item, variantKey);
  // The catalog default acc = 0 is not a usable device value.
  if (item?.id === 'velocity' && Number(values.acc) === 0) values.acc = 60;
  const maxCurrent = Number(limits?.maxCurrentMa);
  if (Number.isFinite(maxCurrent)) {
    for (const key of ['maxCurrentMa', 'currentMa']) {
      if (Number.isFinite(Number(values[key])) && Number(values[key]) > maxCurrent) values[key] = maxCurrent;
    }
  }
  return values;
}
