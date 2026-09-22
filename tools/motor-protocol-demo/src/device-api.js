import { experimentWindowText } from './device-limits.js';
import { expectedDurationMs } from './simulation.js';

// One request helper for the whole device page.
//
// Success: the parsed JSON body.
// HTTP error: an Error whose `status` is the HTTP code — the board answered, so
//   the message is a concrete rejection and the outcome is known.
// Transport failure (offline, DNS, timeout): an Error with `uncertain = true`
//   and message 'request_timeout' / 'network_error' / 'request_cancelled'. Only
//   this case may be described as "结果未知"; never retry automatically.
export async function request(path, data, signal) {
  const controller = new AbortController();
  let timedOut = false;
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  const timer = setTimeout(() => { timedOut = true; abort(); }, 1800);
  try {
    const response = await fetch(path, {
      method: data === undefined ? 'GET' : 'POST', cache: 'no-store',
      body: data === undefined ? undefined : new URLSearchParams(data),
      signal: controller.signal,
    });
    let payload = null;
    try { payload = await response.json(); } catch { payload = null; }
    if (!response.ok) {
      const error = new Error(payload?.error || payload?.message || `HTTP ${response.status}`);
      error.status = response.status;
      // The board's rejection body carries details the message alone loses —
      // the queue endpoints report the offending source line in `line`.
      error.payload = payload;
      throw error;
    }
    if (payload === null) {
      const error = new Error('板端返回内容不是有效 JSON');
      error.status = response.status;
      throw error;
    }
    return payload;
  } catch (cause) {
    if (cause?.status) throw cause;
    const error = new Error(
      signal?.aborted ? 'request_cancelled' : timedOut ? 'request_timeout' : 'network_error',
    );
    error.uncertain = true;
    throw error;
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}

const MIN_CURRENT_MA = 100; // firmware constraintCurrentMa floor

/**
 * Why the board would refuse this frame.
 *
 * The byte layout checks (sync queue, motion mode, preview-only paths) are
 * fixed firmware policy. Value checks use the *confirmed* board limits when
 * they are known: `limits === null` means /api/limits has not answered, and the
 * caller keeps motion disabled instead of pretending 120/240 were loaded.
 */
export function supportReason(bytes, limits = null) {
  if (!bytes?.length) return '请先修正参数';
  const op = bytes[1];
  const word = (index) => bytes[index] * 256 + bytes[index + 1];
  const long = (index) => bytes[index] * 0x1000000 + (bytes[index + 1] << 16) + (bytes[index + 2] << 8) + bytes[index + 3];
  const has = (index, width = 2) => bytes.length >= index + width;
  const window = experimentWindowText(limits);
  const adjust = '可在「调试限制」中调整并保存到板上。';
  const trial = !window
    ? '速度／力矩试验由板端计时'
    : Number(limits.experimentSeconds) === 0
      ? '速度／力矩试验会持续运行，需要手动停止'
      : `速度／力矩试验最多运行 ${limits.experimentSeconds} 秒`;

  if ([0xff, 0xfd].includes(op))
    return `仅预览：此模式尚未接入板端运动监督；相对运动请使用 CD；${trial}。`;
  if (op === 0xfb || op === 0xcb) {
    // Manual V1.0.5 pp54-55: FB is 12 bytes, CB adds the max-current field for
    // 14. Only the immediate form is supervised, and all three documented motion
    // modes are usable. Travel and duration are coordinate dependent, so they
    // are decided by the board (see directPositionBoardNote).
    const label = op === 0xcb ? 'CB' : 'FB';
    const length = op === 0xcb ? 14 : 12;
    // The exact length is checked first, so every index read below exists.
    if (bytes.length !== length)
      return `${label} 为 ${length} 字节：[地址][${label}][方向][速度 2][位置角度 4][运动模式][同步]${op === 0xcb ? '[最大电流 2]' : ''}[6B]。`;
    if (bytes[2] > 1) return '方向只能是 0（CW）或 1（CCW）。';
    if (bytes[9] > 2) return '运动模式只能是 0（相对上一输入目标）／1（绝对坐标零点）／2（相对当前位置）。';
    if (bytes[10] !== 0) return '板端只监督立即执行的直通位置（同步标志必须为 0）；缓存待 FF 触发的方式未接入，可用队列的原始帧下发。';
  }
  if (op === 0xf3 && bytes[4] || op === 0xfe && bytes[3] || [0xf5, 0xc5, 0xf6, 0xc6].includes(op) && bytes[7] || op === 0xcd && bytes[14])
    return '同步队列尚未接入板端监督，请选择立即执行。';
  if (op === 0x9a) {
    // Manual V1.0.5 p61-p62 + ProtocolGate::validateCommand: the supervised
    // trigger is 5 bytes, mode 00-05, sync 00. The cached form still needs the
    // FF trigger, which the board does not supervise.
    if (bytes.length !== 5) return '回零触发为 5 字节：[地址][9A][模式][同步][6B]。';
    if (bytes[2] > 5) return '回零模式只能是 0..5（手册 V1.0.5 p61-p64）。';
    if (bytes[3] !== 0) return '板端只监督立即执行的回零（同步标志必须为 0）；缓存待 FF 触发的方式未接入。';
  }
  if (op === 0xcd && bytes[13] !== 2) return '实机相对运动要求 motionMode = 2。';
  // FB/CB carry no acceleration field: the driver plans the motion itself, so
  // only the zero-speed case is a page-side decision. A zero speed is legal for
  // a zero-travel no-op only. In mode 2 the travel is exactly the angle field,
  // so the page can decide it; in modes 0/1 the travel is resolved by the board
  // against fresh feedback and stays board-resolved.
  if ((op === 0xfb || op === 0xcb) && has(5, 4) && Number(bytes[9]) === 2 && !word(3) && long(5))
    return '速度为 0 时只能是零位移空操作：模式 2 的位移就是位置角度，请填写非零速度。';
  if (op === 0x4c && bytes[18]) return '实机不允许配置上电自动回零。';
  if (op === 0x45) {
    // Manual V1.0.5 p82 (5.6.13): [45][66][save][current u16][6B], 7 bytes. The
    // layout and the documented 0-5000 mA field range are fixed; the configured
    // current policy is applied further down. This is a parameter write, not a
    // motion: it needs a disabled, stationary driver (the board enforces that),
    // but no enable.
    if (bytes.length !== 7) return '闭环最大相电流为 7 字节：[地址][45][66][是否存储][电流 2][6B]。';
    if (bytes[2] !== 0x66) return '0x45 的辅助码固定为 66。';
    if (bytes[3] > 1) return '是否存储只能是 0（不保存）或 1（保存）。';
    if (word(4) > 5000) return `闭环最大相电流 ${word(4)} mA 超出协议范围 0..5000 mA。`;
  }
  if (op === 0x11 && has(4) && word(4) > 0 && word(4) < 30) return '实机反馈间隔为 0（关闭）或至少 30 ms。';

  if (limits) {
    const maxSpeedTenths = Math.round(limits.maxSpeedRpm * 10);
    const maxAngleTenths = Math.round(limits.maxAngleDeg * 10);
    const range = (value, min, max, label, unit) =>
      `${label} ${value} ${unit} 超出板端当前范围 ${min}..${max} ${unit}；${adjust}`;
    const above = (value, max, label, unit) =>
      `${label} ${value} ${unit} 超出板端当前上限 ${max} ${unit}；${adjust}`;

    if (op === 0xf6 || op === 0xc6) {
      if (has(3) && (word(3) < 1 || word(3) > limits.maxAccelRpmS))
        return range(word(3), 1, limits.maxAccelRpmS, '加速度', 'RPM/s');
      if (has(5) && word(5) > maxSpeedTenths)
        return above(word(5) / 10, limits.maxSpeedRpm, '速度', 'RPM');
      if (op === 0xc6 && has(8) && (word(8) < MIN_CURRENT_MA || word(8) > limits.maxCurrentMa))
        return range(word(8), MIN_CURRENT_MA, limits.maxCurrentMa, '电流上限', 'mA');
    }

    if (op === 0xf5 || op === 0xc5) {
      if (has(5) && word(5) > limits.maxCurrentMa)
        return above(word(5), limits.maxCurrentMa, '力矩电流', 'mA');
      if (op === 0xc5 && has(8) && word(8) > maxSpeedTenths)
        return above(word(8) / 10, limits.maxSpeedRpm, '限速', 'RPM');
    }

    if (op === 0x4c) {
      // Manual V1.0.5 p64-p65 makes a 0000-0BB8 RPM velocity and a uint32
      // timeout legal on the wire, so there is no arbitrary cap here: only the
      // configured speed policy (whole RPM, no ×10 scaling) and the current
      // policy apply to the homing parameters.
      if (has(7) && word(6) > limits.maxSpeedRpm)
        return above(word(6), limits.maxSpeedRpm, '回零速度', 'RPM');
      if (has(15) && word(14) > limits.maxCurrentMa)
        return above(word(14), limits.maxCurrentMa, '碰撞检测电流', 'mA');
    }

    if (op === 0xfb || op === 0xcb) {
      // Mirrors buildDirectPositionPlan()/resolveDirectTarget() for everything
      // that does not depend on where the motor currently is.
      if (has(3) && word(3) > maxSpeedTenths)
        return above(word(3) / 10, limits.maxSpeedRpm, '速度', 'RPM');
      // Mode 1 is an absolute coordinate, so the configured travel policy is not
      // applied to it: only the int32 the driver can report back. Modes 0/2
      // carry a displacement, which is bounded like the CD travel field.
      if (has(5, 4) && Number(bytes[9]) === 1) {
        if (long(5) > 0x7fffffff)
          return `绝对目标坐标 ${long(5) / 10} ° 超出当前板端实现的 int32 反馈范围（±214748364.7°）。`;
      } else if (has(5, 4) && long(5) > maxAngleTenths) {
        return above(long(5) / 10, limits.maxAngleDeg, '行程', '°');
      }
      // CB only, and no 100 mA floor: the manual's documented current range is
      // 0000-1388 (0-5000 mA), so every value up to the board policy is legal.
      if (op === 0xcb && has(11) && word(11) > limits.maxCurrentMa)
        return above(word(11), limits.maxCurrentMa, '电流上限', 'mA');
    }

    if (op === 0x45) {
      // The configured current policy is a real ceiling for a parameter write
      // too: the board refuses above it instead of silently clipping.
      if (has(4) && word(4) > limits.maxCurrentMa)
        return above(word(4), limits.maxCurrentMa, '闭环最大相电流', 'mA');
    }

    if (op === 0xcd) {
      if (has(3) && (word(3) < 1 || word(3) > limits.maxAccelRpmS))
        return range(word(3), 1, limits.maxAccelRpmS, '加速度', 'RPM/s');
      if (has(5) && (word(5) < 1 || word(5) > limits.maxAccelRpmS))
        return range(word(5), 1, limits.maxAccelRpmS, '减速度', 'RPM/s');
      if (has(7) && word(7) > maxSpeedTenths)
        return above(word(7) / 10, limits.maxSpeedRpm, '速度', 'RPM');
      if (has(9, 4) && long(9) > maxAngleTenths)
        return above(long(9) / 10, limits.maxAngleDeg, '行程', '°');
      if (has(15) && (word(15) < MIN_CURRENT_MA || word(15) > limits.maxCurrentMa))
        return range(word(15), MIN_CURRENT_MA, limits.maxCurrentMa, '电流上限', 'mA');
      if (has(15) && (!word(7) || !long(9))) return '相对位置运动的速度和行程必须大于 0。';
      if (has(15) && expectedDurationMs(long(9),word(7),word(3),word(5)) > limits.maxMoveSeconds * 1000)
        return `运动预估时长超过板端当前上限 ${limits.maxMoveSeconds} 秒；${adjust}`;
    }
  }

  // Read commands the board accepts and the manual documents (5.5 节). The
  // added codes (1A/26/32/34/39/3C/3D) follow the same list on the board side;
  // 0x21 stays raw; documented bulk 0x42/0x43 replies await reassembly.
  if (![0x1a,0x1f,0x20,0x21,0x24,0x26,0x27,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x39,0x3a,0x3b,0x3c,0x3d,0x42,0x43,0xf3,0xfe,0xcd,0xfb,0xcb,0xf5,0xc5,0xf6,0xc6,0x9a,0x9c,0x0a,0x0e,0x45,0x46,0x93,0x11,0x4c].includes(op))
    return '未知指令只能预览，板端拒绝发送。';
  return null;
}

/**
 * The FB/CB checks the page cannot make, because they depend on where the motor
 * actually is and on the target the driver itself holds. The board resolves them
 * against fresh feedback; the page says so instead of guessing. The text also
 * states what each form can carry: only CB has a current field, so only CB can
 * limit the current of a single command.
 *
 * Returns null for every other opcode.
 */
export function directPositionBoardNote(bytes) {
  const op = Array.isArray(bytes) ? bytes[1] : undefined;
  if (op !== 0xfb && op !== 0xcb) return null;
  // FB carries no current field at all (manual V1.0.5 p54): it cannot impose a
  // per-command current limit, and the board never invents one for it.
  const form = op === 0xcb
    ? 'CB 的电流字段是这条命令的最大电流（手册 0..5000 mA，受板端电流策略约束）。'
    : 'FB 没有电流字段：无法对单条命令限流，需要限流请改用 CB。';
  switch (Number(bytes[9])) {
    case 0:
      return `${form}模式 0 的基准是驱动器目标位置（0x33，手册 p70 的「电机目标位置」）：板端只用新鲜读值解析，没有新鲜读值时直接拒绝并刷新读值；不会用当前实际位置、本地下发过的目标或 0x34 实时设定值（p71，可能是轨迹中间值）代替。`;
    case 1:
      return `${form}模式 1 是绝对坐标：实际行程 = |目标 − 当前位置|，由板端按新鲜反馈判定并受行程／时长上限约束，绝对坐标本身不按行程上限裁剪。`;
    default:
      return `${form}模式 2 的行程由板端按当前实际位置解析；零位移是受监督的空操作，非零位移必须给出非零速度。`;
  }
}

/**
 * Motion commands whose values are validated against the board limit set.
 * 0x9A belongs here: the board supervises homing (ACK 02 + 3B flags), so the
 * trigger is gated on confirmed limits like every other motion.
 */
export const MOTION_OPS = [0xf5, 0xc5, 0xf6, 0xc6, 0xfb, 0xcb, 0xfd, 0xcd, 0x9a];

export const isMotionOpcode = (op) => MOTION_OPS.includes(op);

/**
 * Commands whose values can only be judged against the board's confirmed limit
 * set, so the page keeps them disabled until /api/limits has answered.
 *
 * 0x45 (closed-loop maximum phase current) belongs here but is deliberately NOT
 * a motion opcode: it is a parameter write that needs a disabled, stationary
 * driver instead of an enable, which is why it is a separate list.
 */
export const LIMIT_DEPENDENT_OPS = [...MOTION_OPS, 0x45];
export const isLimitDependentOpcode = (op) => LIMIT_DEPENDENT_OPS.includes(op);

export const stateLabels = { idle:'已使能 · 静止', disabled:'未使能', enabled:'已使能', moving:'运动中', homing:'回零中', stop_requested:'等待停止反馈', enable_pending:'等待使能应答', fault:'故障', experiment_running:'试验运行中' };

/**
 * Homing outcome reported by the controller (`status.homeOutcome`, frozen names
 * from MotorControl::homeOutcomeName). A 02 ACK and a cleared 0x3B bit never
 * prove completion, so every outcome keeps its own wording — `no_motion` is the
 * manual's 12/22 answer (already at the origin or the limit is already
 * triggered): the attempt finished but the motor did not move, which is neither
 * a completion nor a fault.
 */
export const homeOutcomeLabels = {
  none:'尚无回零记录', running:'回零进行中（等待受监督的结果）', done:'回零已完成（板端判定）',
  no_motion:'电机未动：已在零点或限位已触发（12/22），本次没有完成回零',
  cancelled:'回零已取消', failed:'回零失败',
};

/**
 * `homeOrg` is the node's latest 0x3B homing status **byte** (the manual's Org),
 * shown with its documented bit meanings (manual V1.0.5 p62-p63). Bits 6/7 are
 * reserved and never labelled.
 */
export const homeOrgBits = [
  { bit: 0, label: '编码器就绪' }, { bit: 1, label: '校准表就绪' }, { bit: 2, label: '正在回零' },
  { bit: 3, label: '回零失败' }, { bit: 4, label: '过热保护' }, { bit: 5, label: '过流保护' },
];

export function homeOrgText(org) {
  const value = Number(org);
  if (org == null || !Number.isFinite(value)) return null;
  const on = homeOrgBits.filter((entry) => value & (1 << entry.bit)).map((entry) => entry.label);
  return `0x${(value & 0xff).toString(16).toUpperCase().padStart(2, '0')}${on.length ? ` · ${on.join(' / ')}` : ' · 无标志置位'}`;
}

/**
 * One line describing the board's homing state for the motor this status was
 * read for, or null when the status says nothing about it.
 *
 * The outcome is only attributed when `homeId` names the same node: the
 * controller keeps the last run's id, so without that check a different motor's
 * result would be shown here. `homeRunning`/`homeFailed` are decoded from
 * `homeOrg`, and a disagreement between them and the outcome is reported
 * instead of being silently resolved in favour of the nicer one.
 */
export function homeStatusText(status) {
  const homeId = Number(status?.homeId);
  if (!Number.isInteger(homeId) || homeId === 0) return null;
  if (homeId !== Number(status?.id)) return null;
  const outcome = status?.homeOutcome;
  if (!outcome || outcome === 'none') return null;
  const label = homeOutcomeLabels[outcome] || outcome;
  const mode = Number.isFinite(Number(status.homeMode)) ? ` · 模式 ${status.homeMode}` : '';
  const org = homeOrgText(status.homeOrg);
  const bits = org ? ` · 3B ${org}` : '';
  const conflict = (status.homeRunning === true && outcome !== 'running')
    ? '（3B bit2 仍报告正在回零，与结果不一致，按未完成看待）'
    : (status.homeFailed === true && outcome === 'done')
      ? '（3B bit3 报告回零失败，与结果不一致，按失败看待）'
      : '';
  return `${label}${mode}${bits}${conflict}`;
}

// ---------------------------------------------------------------------------
// Per-ID rotation distance (mm/rev, board NVS)
// ---------------------------------------------------------------------------

export const ROTATION_DISTANCE_MIN = 0.000001;
export const ROTATION_DISTANCE_MAX = 1000000;

/**
 * Read GET/POST /api/motor-distance. The response is only adopted when it
 * carries the requested address and a value the board itself would accept
 * (positive and inside the documented range, or exactly 0/null for "cleared").
 * A mismatched or out-of-range answer is rejected rather than assigned to
 * whatever ID happens to be selected — a wrong profile is worse than none.
 */
export function readMotorDistance(payload, expectedId = null) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) {
    return { ok: false, error: '板端返回的旋转距离不是有效对象' };
  }
  const id = Number(payload.id);
  if (!Number.isInteger(id) || id < 1 || id > 255) {
    return { ok: false, error: `板端返回的电机地址无效：${payload.id}` };
  }
  if (expectedId != null && id !== Number(expectedId)) {
    return { ok: false, mismatch: true, error: `板端返回的电机地址（${id}）与请求的 ${expectedId} 不一致，本地未采用该结果。` };
  }
  if (!Object.prototype.hasOwnProperty.call(payload, 'rotationDistance')) {
    return { ok: false, error: '板端返回缺少 rotationDistance 字段' };
  }
  const raw = payload.rotationDistance;
  // The contract is a number or null; anything else is a malformed answer, not
  // a value to be coerced into one.
  if (raw === null) return { ok: true, id, value: null };
  if (typeof raw !== 'number') return { ok: false, error: `板端返回的旋转距离不是数字：${JSON.stringify(raw)}` };
  if (raw === 0) return { ok: true, id, value: null };
  if (!Number.isFinite(raw) || raw < ROTATION_DISTANCE_MIN || raw > ROTATION_DISTANCE_MAX) {
    return { ok: false, error: `板端返回的旋转距离超出 ${ROTATION_DISTANCE_MIN}..${ROTATION_DISTANCE_MAX} mm/rev 的范围：${raw}` };
  }
  return { ok: true, id, value: raw };
}

// ---------------------------------------------------------------------------
// Board queue (编排队列)
// ---------------------------------------------------------------------------

export const QUEUE_STATES = ['idle', 'running', 'done', 'failed', 'cancelled'];

export const queueStateLabels = {
  idle:'空闲', running:'运行中', done:'已完成', failed:'失败', cancelled:'已取消', unknown:'状态未知',
};

export const queueActionLabels = {enable:'使能',disable:'失能',move:'相对移动',home:'回零',torque:'限速力矩',velocity:'限流速度',stop:'停止',wait:'等待',hex:'逻辑帧直通',can:'CAN 帧直通',none:'—'};
export function queueMessageText(message) {
  return ({idle:'尚未执行',running:'正在执行',done:'已完成受监督的动作',raw_frames_submitted:'程序发送结束；原始帧不判断机械动作完成',frames_submitted:'程序发送结束；原始帧不判断机械动作完成',cancelled:'已取消后续步骤并请求停止',stopped:'已停止队列',uart_stop:'已由串口停止队列',home_no_motion:'驱动报告已在零点或限位触发，本次未运动'})[message] || errorLabels[message] || message;
}

/**
 * Read GET/POST /api/queue. A payload that does not carry a known state is
 * rejected instead of being reported as idle: "no answer" and "nothing is
 * running" are different facts, and only the board may claim the latter.
 */
export function readQueueStatus(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload)) {
    return { ok: false, error: '板端返回的队列状态不是有效对象' };
  }
  const state = String(payload.state ?? '');
  if (!QUEUE_STATES.includes(state)) return { ok: false, error: `板端返回的队列状态无法识别：${state || '（缺失）'}` };
  const number = (key, fallback = 0) => (Number.isFinite(Number(payload[key])) ? Number(payload[key]) : fallback);
  return {
    ok: true,
    status: {
      state,
      runId: number('runId'),
      step: number('step'),
      total: number('total'),
      iteration: number('iteration'),
      repeat: number('repeat'),
      line: number('line'),
      action: typeof payload.action === 'string' ? payload.action : '',
      message: typeof payload.message === 'string' ? payload.message : '',
      raw: payload.raw === true,
    },
  };
}

/** Progress line for the queue: step/iteration/line, never an optimistic done. */
export function queueProgressText(status) {
  if (!status) return '尚未读取到队列状态';
  const label = queueStateLabels[status.state] || status.state;
  const step = status.step > 0 ? `第 ${status.step}/${status.total || '?'} 步` : '尚未开始第一步';
  const iteration = status.repeat > 1 ? ` · 第 ${status.iteration}/${status.repeat} 轮` : '';
  const line = status.line > 0 ? ` · 源程序第 ${status.line} 行` : '';
  return `${label} · ${step}${iteration}${line}`;
}

/**
 * Opcodes that stay usable while a queue runs. Reads stay accessible, and the
 * board cancels the queue first for stop (FE) and the homing interrupt (9C).
 * 0xF3 is deliberately absent: only its disable form is allowed (see below),
 * because a manual enable would collide with the enable state the running
 * program owns — the board rejects it.
 */
export const QUEUE_ALLOWED_OPS = [
  0x1a, 0x1f, 0x20, 0x21, 0x24, 0x26, 0x27, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
  0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x42, 0x43, 0xfe, 0x9c,
];

/**
 * Why a manual command is locked while the queue owns the board.
 *
 * `bytes` is the whole logical frame, not just the function code: 0xF3 carries
 * both enable and disable in byte 3, and only the disable direction may be sent
 * while a program runs. `unknown` means a submission was never confirmed, so
 * the page cannot prove the bus is free either — it reports that instead of
 * pretending the queue is idle.
 */
export function queueConflictReason(bytes, { running = false, unknown = false } = {}) {
  if (!running && !unknown) return null;
  const opcode = Array.isArray(bytes) ? bytes[1] : undefined;
  const disabling = opcode === 0xf3 && bytes[3] === 0;
  if (QUEUE_ALLOWED_OPS.includes(opcode) || disabling) return null;
  const tail = '读取、停止与失能（不是使能）仍可用。';
  if (opcode === 0xf3) {
    return running
      ? `板端队列正在运行：手动使能会被板端拒绝（会使队列的使能状态失效），请先「取消队列」。${tail}`
      : `队列提交结果未知：手动使能会被板端拒绝，请先「取消队列」核对状态。${tail}`;
  }
  return running
    ? `板端队列正在运行：普通指令已锁定，请先「取消队列」，或使用「全部停止」（板端会先取消队列）。${tail}`
    : `队列提交结果未知：执行状态尚未确认，请先「取消队列」核对后再发送普通指令。${tail}`;
}

export const errorLabels = {
  wifi_busy:'Wi-Fi 正忙，请稍后再试', busy:'设备忙，请先停止并关闭使能',
  not_enabled:'尚未收到使能确认', can_unavailable:'CAN 控制器不可用', can_tx_failed:'CAN 发送失败，检查接线与终端电阻',
  feedback_unavailable:'缺少新鲜位置／速度反馈', feedback_stale:'电机反馈中断', stop_pending:'等待真实静止反馈',
  enable_and_wait_for_stationary_feedback:'请先使能，并等待静止反馈',
  disable_and_wait_for_stationary_feedback:'配置前请关闭使能，并等待真实静止反馈',
  unsupported_or_invalid_command:'指令不受支持或超出板端参数范围',
  motion_active:'电机正在运动，请先停止再操作',
  uart_operation_active:'串口操作正在进行，请稍后再试',
  limits_busy:'设备正忙（有运动或串口操作），限制未保存',
  limits_save_failed:'板端保存 NVS 失败，实际限制未改变',
  limits_invalid:'限制值超出可设置范围或格式不正确',
  speed_out_of_range:'速度超出当前调试限制，请查看「调试限制」',
  accel_out_of_range:'加速度超出当前调试限制，请查看「调试限制」',
  decel_out_of_range:'减速度超出当前调试限制，请查看「调试限制」',
  current_out_of_range:'电流超出当前调试限制，请查看「调试限制」',
  angle_out_of_range:'行程超出当前调试限制，请查看「调试限制」',
  duration_too_long:'预估运动时长超出当前调试限制，请查看「调试限制」',
  // Direct (FB/CB) position: board-side reasons the page cannot pre-empt.
  target_not_fresh:'尚未读取到驱动器目标位置（0x33，手册 p70）：模式 0 需要新鲜读值，板端已请求刷新，请稍后重试',
  travel_out_of_range:'实际行程（目标位置与当前实际位置之差）超出当前调试限制，请查看「调试限制」',
  target_out_of_range:'解析后的目标位置超出当前板端实现的 int32 反馈范围，请调整坐标或行程',
  speed_required:'非零位移必须给出非零速度；速度为 0 只允许零位移的空操作',
  direction_invalid:'方向只能是 0（CW）或 1（CCW）',
  motion_mode_invalid:'运动模式只能是 0（相对上一输入目标）／1（绝对坐标零点）／2（相对当前位置）',
  direct_sync_not_supported:'板端只监督立即执行的直通位置（FB/CB）；缓存待 FF 触发的方式未接入，可用队列的原始帧下发',
  // Homing parameter writes (0x4C) and homing supervision (0x9A).
  home_current_out_of_range:'碰撞检测电流超出板端当前电流策略，请查看「调试限制」',
  home_velocity_out_of_range:'回零速度超出协议上限 3000 RPM',
  home_velocity_out_of_policy:'回零速度超出板端当前速度策略，请查看「调试限制」',
  power_on_homing_not_supported:'板端暂不支持配置上电自动回零（可用队列里的原始帧显式下发 4C）',
  home_rejected:'板端拒绝了回零触发',
  home_timeout:'回零超时，板端已退出并请求停止',
  home_failed:'回零失败（驱动器 3B 报告失败标志）',
  homing_busy:'已有受监督的动作在进行，请等待结束或停止',
  // Board queue.
  queue_busy:'队列已在运行或即将开始时被拒绝，请先取消队列',
  queue_active:'队列正在运行：请先取消队列再操作',
  queue_invalid:'队列程序不合法，板端已拒绝整份程序',
  queue_too_long:'队列程序超过板端上限（64 个动作 / 8192 字节）',
  queue_not_available:'板端没有队列接口（固件未更新）',
  program_line:'程序在标注的源程序行被拒绝',
  argument_count:'参数数量不正确，请检查这一行的写法',
  invalid_integer:'这里需要十进制整数', invalid_number:'数字格式不正确',
  rotation_distance_missing:'该电机未保存 rotation distance，不能换算毫米',
  rotation_distance_invalid:'该电机的 rotation distance 无效',
  move_angle_out_of_range:'换算后的行程超出当前调试限制',
  move_angle_rounds_to_zero:'行程换算后不足一个 0.1° 计数',
  timed_value_zero:'速度／力矩动作不能为零，请使用 stop',
  timed_speed_out_of_range:'试验转速超出当前调试限制',
  timed_current_out_of_range:'试验电流超出当前调试限制',
  timed_accel_out_of_range:'试验加速度超出当前调试限制',
  target_feedback_timeout:'等待目标电机反馈超时，后续步骤已取消',
  stop_unconfirmed:'未能确认静止，后续步骤已取消',
  home_status_missing:'缺少本次回零状态反馈，已请求中断并停止',
  home_ack_timeout:'等待回零应答超时，已请求中断并停止',
  home_protection:'回零时驱动报告过温或过流保护',
  raw_tx_failed:'原始帧发送失败，后续步骤已取消',
  fault_active:'板端存在故障，队列已中止',
  motor_distance_unavailable:'板端不支持读取／保存旋转距离（固件未更新）',
  motor_distance_busy:'设备正忙（队列或其它操作进行中），旋转距离未保存',
  motor_distance_invalid:'旋转距离需在 0.000001..1000000 mm/rev 之间，或写 0 清除',
  request_timeout:'请求超时', network_error:'网络错误或设备未连接', request_cancelled:'请求已取消',
};
