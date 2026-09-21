// Simulation-only model of one X42S/X28S motor plus the wire codecs needed to
// show believable TX/RX traffic. Nothing here talks to hardware: every response
// is generated locally and is labelled as simulated in the UI.
//
// Feedback frame layouts and the ack status bytes are copied from
// references/MotionCore.h; the trapezoid duration math is a direct port of
// motion::expectedDurationMs().

import {
  MANUAL_LIMITS,
  PROTOCOL_CHECKSUM,
  TENTHS_PER_DEGREE,
  TENTHS_PER_RPM,
  hexByte,
} from './protocol.js';

export const ACK_STATUS = {
  received: 0x02,
  completed: 0x9f,
  parameterError: 0xe2,
  formatError: 0xee,
};

const ACK_TEXT = {
  [ACK_STATUS.received]: '已接收指令',
  [ACK_STATUS.completed]: '已完成',
  [ACK_STATUS.parameterError]: '参数错误',
  [ACK_STATUS.formatError]: '格式错误',
};

export function classifyAck(status) {
  return ACK_TEXT[status] ?? `未知状态 0x${hexByte(status)}`;
}

// Demo bounds so a simulation can never run away with the UI.
export const MOTION_DEMO_MIN_MS = 300;
export const MOTION_DEMO_MAX_MS = 10000;
export const VELOCITY_DEMO_WINDOW_MS = 10000;
export const FEEDBACK_DEMO_MIN_INTERVAL_MS = 250;
export const MOTION_TICK_MS = 80;

// ---------------------------------------------------------------------------
// Feedback codecs (MotionCore.h)
// ---------------------------------------------------------------------------

export function decodeFeedbackFrame(data) {
  if (!Array.isArray(data) || data.length === 0) return null;
  switch (data[0]) {
    case 0x27: {
      if (data.length !== 4 || data[3] !== PROTOCOL_CHECKSUM) return null;
      const value = (data[1] << 8) | data[2];
      return { field: 'current', value, unit: 'mA', text: `电流 ${value} mA` };
    }
    case 0x35: {
      if (data.length !== 5 || data[4] !== PROTOCOL_CHECKSUM) return null;
      if (data[1] > 1) return null;
      const magnitude = (data[2] << 8) | data[3];
      const value = data[1] === 0 ? magnitude : -magnitude;
      return {
        field: 'velocity',
        value,
        unit: '0.1 RPM',
        text: `速度 ${(value / 10).toFixed(1)} RPM`,
      };
    }
    case 0x36: {
      if (data.length !== 7 || data[6] !== PROTOCOL_CHECKSUM) return null;
      if (data[1] > 1) return null;
      const magnitude =
        data[2] * 0x1000000 + (data[3] << 16) + (data[4] << 8) + data[5];
      if (magnitude > 0x7fffffff) return null;
      const value = data[1] === 0 ? magnitude : -magnitude;
      return {
        field: 'position',
        value,
        unit: '0.1°',
        text: `位置 ${(value / 10).toFixed(1)}°`,
      };
    }
    case 0x3a: {
      if (data.length !== 3 || data[2] !== PROTOCOL_CHECKSUM) return null;
      return {
        field: 'flags',
        value: data[1],
        unit: '',
        text: `标志 0x${hexByte(data[1])}`,
      };
    }
    default:
      return null;
  }
}

/** `<function>[status]0x6B` acknowledgement frame. */
export function parseAckFrame(data) {
  if (!Array.isArray(data) || data.length !== 3) return null;
  if (data[2] !== PROTOCOL_CHECKSUM) return null;
  return { opcode: data[0], status: data[1], text: classifyAck(data[1]) };
}

export function describeFrame(data) {
  const feedback = decodeFeedbackFrame(data);
  if (feedback) return { kind: 'feedback', ...feedback };
  const ack = parseAckFrame(data);
  if (ack) return { kind: 'ack', ...ack };
  return { kind: 'unknown', text: '无解码器' };
}

export function ackFrame(opcode, status = ACK_STATUS.received) {
  return [opcode, status, PROTOCOL_CHECKSUM];
}

function signedMagnitudeFrame(opcode, value, magnitudeBytes) {
  const sign = value < 0 ? 1 : 0;
  const magnitude = Math.abs(value);
  const bytes = [opcode, sign];
  for (let index = magnitudeBytes - 1; index >= 0; index -= 1) {
    bytes.push((magnitude >>> (8 * index)) & 0xff);
  }
  bytes.push(PROTOCOL_CHECKSUM);
  return bytes;
}

export const encodePositionFrame = (tenths) => signedMagnitudeFrame(0x36, tenths, 4);
export const encodeVelocityFrame = (tenths) => signedMagnitudeFrame(0x35, tenths, 2);

export function encodeCurrentFrame(mA) {
  return [0x27, (mA >> 8) & 0xff, mA & 0xff, PROTOCOL_CHECKSUM];
}

export function encodeFlagsFrame(flags) {
  return [0x3a, flags & 0xff, PROTOCOL_CHECKSUM];
}

/** Port of motion::expectedDurationMs() from references/MotionCore.h. */
export function expectedDurationMs(distanceTenths, speedTenths, accelRpmPerSec, decelRpmPerSec) {
  if (!distanceTenths || !speedTenths || !accelRpmPerSec || !decelRpmPerSec) return 0;
  const distance = distanceTenths / 10;
  const speed = (speedTenths / 10) * 6; // deg/s, 1 RPM == 6 deg/s
  const accel = accelRpmPerSec * 6;
  const decel = decelRpmPerSec * 6;

  const accelDistance = (speed * speed) / (2 * accel);
  const decelDistance = (speed * speed) / (2 * decel);

  let seconds;
  if (accelDistance + decelDistance <= distance) {
    seconds =
      speed / accel + speed / decel + (distance - accelDistance - decelDistance) / speed;
  } else {
    const peak = Math.sqrt((2 * distance * accel * decel) / (accel + decel));
    seconds = peak / accel + peak / decel;
  }
  if (!(seconds > 0) || seconds > 100000) return 0;
  return Math.ceil(seconds * 1000);
}

/** Linear estimate used when the command has no accel/decel fields. */
function linearDurationMs(distanceTenths, speedTenths) {
  if (!distanceTenths || !speedTenths) return 0;
  return Math.ceil((distanceTenths * 1000) / (speedTenths * 6));
}

function boundedDuration(distanceTenths, speedTenths, accelRpmS, decelRpmS) {
  let raw = 0;
  if (accelRpmS > 0 && decelRpmS > 0) {
    raw = expectedDurationMs(distanceTenths, speedTenths, accelRpmS, decelRpmS);
  }
  if (!raw) raw = linearDurationMs(distanceTenths, speedTenths);
  if (!raw) return { durationMs: 0, clamped: false };
  if (raw < MOTION_DEMO_MIN_MS) return { durationMs: MOTION_DEMO_MIN_MS, clamped: true };
  if (raw > MOTION_DEMO_MAX_MS) return { durationMs: MOTION_DEMO_MAX_MS, clamped: true };
  return { durationMs: raw, clamped: false };
}

// ---------------------------------------------------------------------------
// Per-motor simulation state
// ---------------------------------------------------------------------------

export const SEED_ADDRESS = 1;
export const SEED_POSITION_TENTHS = 1800; // 180.0 degree, matches the design mock
export const SEED_CURRENT_MA = 210;
export const SEED_RESPONSE = [0xf3, 0x02, 0x6b];
export const SEED_FEEDBACK = [0x36, 0x00, 0x00, 0x00, 0x07, 0x08, 0x6b];

export function createMotor(address, overrides = {}) {
  return {
    address,
    enabled: false,
    positionTenths: 0,
    velocityTenths: 0,
    currentMa: 0,
    flags: 0,
    ctrlMode: 0,
    feedbackIntervalMs: 0,
    syncQueue: [],
    lastResponse: null,
    status: { kind: 'idle', text: '暂无应答' },
    ...overrides,
  };
}

/** Motor 1 starts from the sample data shown in the reference design. */
export function createSeedMotor(address = SEED_ADDRESS) {
  return createMotor(address, {
    enabled: true,
    positionTenths: SEED_POSITION_TENTHS,
    velocityTenths: 0,
    currentMa: SEED_CURRENT_MA,
    flags: 0x01,
    lastResponse: {
      data: [...SEED_RESPONSE],
      kind: 'ack',
      text: '已接收指令',
      simulated: true,
    },
    status: { kind: 'received', text: '已接收指令' },
  });
}

export function createSeedRecords(baseTime, address = SEED_ADDRESS) {
  const frameId = (address << 8) | 0;
  const row = (offsetMs, dir, dlc, data, note) => ({
    id: `seed-${offsetMs}-${dir}`,
    at: baseTime + offsetMs,
    dir,
    canId: frameId,
    dlc,
    data,
    note,
    address,
    simulated: true,
  });
  return [
    row(0, 'TX', 5, [0xf3, 0xab, 0x01, 0x00, 0x6b], '使能请求'),
    row(12, 'RX', 3, [...SEED_RESPONSE], '已接收'),
    row(40, 'RX', 7, [...SEED_FEEDBACK], '位置 180.0°'),
  ];
}

export function positionDegrees(tenths) {
  return tenths / TENTHS_PER_DEGREE;
}

export function formatPosition(tenths) {
  return `${positionDegrees(tenths).toFixed(1)}°`;
}

export function formatVelocity(tenths) {
  return `${(tenths / 10).toFixed(1)} RPM`;
}

export function formatCurrent(mA) {
  return `${Math.round(mA)} mA`;
}

// ---------------------------------------------------------------------------
// Command effects
// ---------------------------------------------------------------------------

const READ_DECODERS = {
  readCpos: 'position',
  readVel: 'velocity',
  readCpha: 'current',
  readFlag: 'flags',
};

const MOTION_VARIANTS = new Set([
  'position',
  'passthroughPosition',
  'velocity',
  'torque',
]);

// MotionCore.h kMotionModeRelativeToCurrent: the only motion mode the sources
// define, and therefore the only one this prototype simulates.
export const MOTION_MODE_RELATIVE = 2;

// Extended CAN identifier used by the broadcast "stop everything" request.
export const BROADCAST_CAN_ID = 0x00000000;

/**
 * Decide what a command should do to the simulated motor.
 * Pure: the caller owns timers and state transitions.
 */
export function planCommandEffect({ item, variantKey, values }) {
  const sync = Number(values?.sync ?? 0) === 1;

  if (item.custom) {
    return {
      kind: 'unsupported',
      ackStatus: null,
      cancelsMotion: false,
      note: '未知功能码：只发送 TX 帧，无解码器、不做模拟。',
    };
  }

  switch (item.id) {
    case 'enable':
      return {
        kind: values.state === 1 ? 'enable' : 'disable',
        enabled: values.state === 1,
        sync,
        ackStatus: ACK_STATUS.received,
        cancelsMotion: true,
        note: values.state === 1
          ? '使能状态已按模拟更新。'
          : '失能会清空待触发队列并停止模拟运动。',
      };
    case 'stop':
      return {
        kind: 'stop',
        sync,
        ackStatus: ACK_STATUS.received,
        cancelsMotion: true,
        note: '停止会作废未完成的模拟运动与待触发队列。',
      };
    case 'syncTrigger':
      return {
        kind: 'sync-trigger',
        ackStatus: ACK_STATUS.received,
        cancelsMotion: true,
        note: '执行此前排队等待触发的模拟指令。',
      };
    case 'resetCurPosToZero':
      return {
        kind: 'zero-position',
        positionTenths: 0,
        ackStatus: ACK_STATUS.received,
        cancelsMotion: true,
        note: '模拟中把当前位置计数置 0（不移动电机）。',
      };
    case 'modifyCtrlMode':
      return {
        kind: 'config',
        ctrlMode: Number(values.ctrlMode ?? 0),
        ackStatus: ACK_STATUS.received,
        cancelsMotion: false,
        note: '控制模式取值表未在源码中定义，演示只记录写入值。',
      };
    case 'realtimePositionFeedback':
      return {
        kind: 'config',
        feedbackIntervalMs: Number(values.intervalMs ?? 0),
        ackStatus: ACK_STATUS.received,
        cancelsMotion: false,
        note: '非 0 间隔会启动模拟的周期性位置反馈帧。',
      };
    case 'resetClogProtection':
    case 'originSetZero':
    case 'originModifyParams':
    case 'originTriggerReturn':
    case 'originInterrupt':
      return {
        kind: 'device-note',
        ackStatus: ACK_STATUS.received,
        cancelsMotion: false,
        note: '网页未接入该指令的物理效果：只发送真实帧并给出模拟说明，不移动位置、不伪造回零成功。',
      };
    default:
      break;
  }

  if (item.groupId === 'read') {
    const decoder = READ_DECODERS[item.id] ?? null;
    return decoder
      ? {
        kind: 'read',
        decoder,
        ackStatus: ACK_STATUS.received,
        cancelsMotion: false,
        note: '按源码定义的应答布局生成模拟数据帧，不改动运动状态。',
      }
      : {
        kind: 'read',
        decoder: null,
        ackStatus: null,
        cancelsMotion: false,
        note: '应答数据布局未在源码中定义：只发送 TX 帧，不构造数据帧、不回执。',
      };
  }

  if (item.id === 'position') {
    const motionMode = Number(values.motionMode ?? 0);
    if (motionMode !== MOTION_MODE_RELATIVE) {
      return {
        kind: 'not-modelled',
        ackStatus: null,
        cancelsMotion: true,
        note: `运动模式 ${motionMode} 的取值含义未在源码中定义：只发送帧，不模拟位移与完成回执。`,
      };
    }
    const distance = Number(values.clk ?? 0);
    const speedTenths = Number(values.vel ?? 0);
    const accel = Number(values.accel ?? 0);
    const decel = Number(values.decel ?? 0);
    const direction = Number(values.dir ?? 0) === 1 ? -1 : 1;
    const { durationMs, clamped } = boundedDuration(distance, speedTenths, accel, decel);
    return {
      kind: 'motion',
      sync,
      ackStatus: ACK_STATUS.received,
      cancelsMotion: true,
      motion: durationMs
        ? {
          mode: 'bounded',
          durationMs,
          deltaTenths: direction * distance,
          speedTenths,
          currentMa: Number(values.maxCurrentMa ?? values.currentMa ?? 0),
          clamped,
        }
        : null,
      note: durationMs
        ? `${clamped ? '演示时长已按仿真窗口调整；' : ''}运动为有界模拟（梯形相对位置），停止或新指令会立即作废。`
        : '速度为 0，演示不产生位移（模拟）。',
    };
  }

  if (item.id === 'passthroughPosition') {
    return {
      kind: 'not-modelled',
      ackStatus: null,
      cancelsMotion: true,
      note: '直通位置的行程单位（clk）未在源码中定义：只发送帧，不模拟位移与完成回执。',
    };
  }

  if (item.id === 'velocity') {
    const speedTenths = Number(values.vel ?? 0);
    if (speedTenths === 0) {
      return {
        kind: 'motion',
        sync,
        ackStatus: ACK_STATUS.received,
        cancelsMotion: true,
        motion: null,
        note: '速度为 0，演示不产生运动。',
      };
    }
    return {
      kind: 'motion',
      sync,
      ackStatus: ACK_STATUS.received,
      cancelsMotion: true,
      motion: {
        mode: 'continuous',
        durationMs: VELOCITY_DEMO_WINDOW_MS,
        speedTenths,
        direction: Number(values.dir ?? 0) === 1 ? -1 : 1,
        currentMa: Number(values.maxCurrentMa ?? 0),
      },
      note: `速度模式为连续运动：演示最多模拟 ${VELOCITY_DEMO_WINDOW_MS / 1000} 秒后自动停转（模拟限制）。`,
    };
  }

  if (item.id === 'torque') {
    return {
      kind: 'torque',
      sync,
      ackStatus: ACK_STATUS.received,
      cancelsMotion: true,
      currentMa: Number(values.currentMa ?? 0),
      note: '电流环力矩指令按 mA 下发：演示只更新电流读数，不建模机械运动。',
    };
  }

  return {
    kind: 'unknown',
    ackStatus: ACK_STATUS.received,
    cancelsMotion: false,
    note: '演示未建模该指令的效果。',
  };
}

export function motionSummary(plan) {
  if (!plan?.motion) return '';
  if (plan.motion.mode === 'continuous') {
    return `连续运动模拟 ${plan.motion.durationMs / 1000}s · ${(plan.motion.speedTenths / 10).toFixed(1)} RPM`;
  }
  return `有界运动模拟 ${plan.motion.durationMs} ms · 行程 ${(plan.motion.deltaTenths / 10).toFixed(1)}°`;
}

export function feedbackFrameFor(decoder, motor) {
  switch (decoder) {
    case 'position':
      return encodePositionFrame(motor.positionTenths);
    case 'velocity':
      return encodeVelocityFrame(motor.velocityTenths);
    case 'current':
      return encodeCurrentFrame(motor.currentMa);
    case 'flags':
      return encodeFlagsFrame(motor.enabled ? 0x01 : 0x00);
    default:
      return null;
  }
}

export function isMotionItem(itemId) {
  return MOTION_VARIANTS.has(itemId);
}

// ---------------------------------------------------------------------------
// 常规试动 move planner
//
// Mirrors motion::buildMovePlan() from references/MotionCore.h: nothing is
// clipped silently, every rejected value gets a reason.
// ---------------------------------------------------------------------------

export const MANUAL_DEFAULTS = {
  angle: '90',
  dir: 0,
  speed: '30.0',
  accel: '60',
  decel: '60',
  current: '800',
};

function parseNumberField(raw, label, errors, key) {
  const text = String(raw ?? '').trim();
  if (!text) {
    errors[key] = `${label}不能为空`;
    return null;
  }
  const value = Number(text);
  if (!Number.isFinite(value)) {
    errors[key] = `${label}必须是有限数字（不接受 NaN / Infinity）`;
    return null;
  }
  return value;
}

function requireInteger(value, label, min, max, errors, key) {
  if (value === null) return null;
  if (!Number.isInteger(value)) {
    errors[key] = `${label}必须是整数`;
    return null;
  }
  if (value < min || value > max) {
    errors[key] = `${label}需在 ${min}..${max} 之间（不做静默截断）`;
    return null;
  }
  return value;
}

export function planManualMove(input) {
  const errors = {};

  const angle = parseNumberField(input.angle, '相对角度', errors, 'angle');
  const speed = parseNumberField(input.speed, '速度', errors, 'speed');
  const accelRaw = parseNumberField(input.accel, '加速度', errors, 'accel');
  const decelRaw = parseNumberField(input.decel, '减速度', errors, 'decel');
  const currentRaw = parseNumberField(input.current, '电流上限', errors, 'current');
  // Direction comes from the direction control only: the angle stays a positive
  // magnitude exactly like MotionCore's MoveRequest.
  const dir = Number(input.dir) === 1 ? 1 : 0;

  let magnitudeTenths = null;
  if (angle !== null) {
    // Finite raw range first, then round, like motion::buildMovePlan().
    if (angle < MANUAL_LIMITS.minAbsAngleDeg || angle > MANUAL_LIMITS.maxAbsAngleDeg) {
      errors.angle = `相对角度需在 ${MANUAL_LIMITS.minAbsAngleDeg}..${MANUAL_LIMITS.maxAbsAngleDeg}° 之间（正数幅值，方向由方向字段决定）`;
    } else {
      magnitudeTenths = Math.round(angle * TENTHS_PER_DEGREE);
      if (magnitudeTenths === 0) errors.angle = '相对角度四舍五入后为 0，请填写不小于 0.1° 的值';
    }
  }

  let speedTenths = null;
  if (speed !== null) {
    if (speed < MANUAL_LIMITS.minSpeedRpm || speed > MANUAL_LIMITS.maxSpeedRpm) {
      errors.speed = `速度需在 ${MANUAL_LIMITS.minSpeedRpm}..${MANUAL_LIMITS.maxSpeedRpm} RPM 之间`;
    } else {
      speedTenths = Math.round(speed * TENTHS_PER_RPM);
      if (speedTenths === 0) errors.speed = '速度四舍五入后为 0，请填写不小于 0.1 RPM 的值';
    }
  }

  const accel = requireInteger(
    accelRaw, '加速度', MANUAL_LIMITS.minAccelRpmS, MANUAL_LIMITS.maxAccelRpmS, errors, 'accel',
  );
  const decel = requireInteger(
    decelRaw, '减速度', MANUAL_LIMITS.minAccelRpmS, MANUAL_LIMITS.maxAccelRpmS, errors, 'decel',
  );
  const current = requireInteger(
    currentRaw, '电流上限', MANUAL_LIMITS.minCurrentMa, MANUAL_LIMITS.maxCurrentMa, errors, 'current',
  );

  if (Object.keys(errors).length > 0) {
    return { ok: false, errors, plan: null, targetTenths: null, deltaTenths: 0, durationMs: 0 };
  }

  const durationMs = expectedDurationMs(magnitudeTenths, speedTenths, accel, decel);
  if (durationMs === 0 || durationMs > MANUAL_LIMITS.maxExpectedDurationMs) {
    return {
      ok: false,
      errors: { speed: '按当前速度与加减速估算的时长超出 60 秒上限，请提高速度或减小行程' },
      plan: null,
      targetTenths: null,
      deltaTenths: 0,
      durationMs: 0,
    };
  }

  return {
    ok: true,
    errors: {},
    durationMs,
    deltaTenths: dir === 1 ? -magnitudeTenths : magnitudeTenths,
    plan: {
      dir,
      accel,
      decel,
      vel: speedTenths,
      clk: magnitudeTenths,
      motionMode: 2, // MotionCore kMotionModeRelativeToCurrent
      sync: 0,
      maxCurrentMa: current,
    },
  };
}
