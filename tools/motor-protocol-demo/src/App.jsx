import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { CommandLibrary } from './components/CommandLibrary.jsx';
import { CommandPanel } from './components/CommandPanel.jsx';
import { FeedbackPanel } from './components/FeedbackPanel.jsx';
import { ManualPanel } from './components/ManualPanel.jsx';
import { Toolbar } from './components/Toolbar.jsx';
import { TracePanel } from './components/TracePanel.jsx';

import {
  COMMAND_GROUPS,
  COMMAND_ITEMS,
  PROTOCOL_CHECKSUM,
  buildFrames,
  defaultValues,
  encodeCommand,
  formatBytes,
  formatCanId,
  frameAnnotation,
  getCommandItem,
  getVariant,
  hexByte,
  identifyCommandBytes,
  parseLogicalHex,
  validateAddress,
} from './protocol.js';

import {
  ACK_STATUS,
  BROADCAST_CAN_ID,
  FEEDBACK_DEMO_MIN_INTERVAL_MS,
  MANUAL_DEFAULTS,
  MOTION_TICK_MS,
  SEED_ADDRESS,
  ackFrame,
  classifyAck,
  createMotor,
  createSeedMotor,
  createSeedRecords,
  decodeFeedbackFrame,
  encodePositionFrame,
  feedbackFrameFor,
  planCommandEffect,
  planManualMove,
} from './simulation.js';

const MAX_RECORDS = 400;
const FEEDBACK_DEMO_MAX_FRAMES = 20;
const ACK_DELAY_MS = 12;
const FEEDBACK_DELAY_MS = 26;
const APP_START = Date.now();
const RAW_FALLBACK_LABELS = (bytes) =>
  bytes.map((_, index) => {
    if (index === 0) return '电机地址';
    if (index === 1) return '功能码';
    if (index === bytes.length - 1) return '固定校验';
    return '参数';
  });

const TABS = [
  { id: 'manual', label: '常规试动' },
  { id: 'lab', label: '指令实验室' },
];

export function App() {
  // --- toolbar state -------------------------------------------------------
  const [address, setAddress] = useState(SEED_ADDRESS);
  const [addressDraft, setAddressDraft] = useState('01');
  const [bitrate, setBitrate] = useState('500');
  const [connected, setConnected] = useState(true);

  // --- navigation ----------------------------------------------------------
  const [tab, setTab] = useState('lab');
  const [query, setQuery] = useState('');
  const [openGroups, setOpenGroups] = useState(() =>
    Object.fromEntries(COMMAND_GROUPS.map((group) => [group.id, Boolean(group.open)])),
  );

  // --- command lab ---------------------------------------------------------
  const [selectedId, setSelectedId] = useState('enable');
  const [variantKey, setVariantKey] = useState('base');
  const [values, setValues] = useState(() => defaultValues(getCommandItem('enable'), 'base'));
  const [editorMode, setEditorMode] = useState('form');
  const [rawText, setRawText] = useState('');
  const [rawDirty, setRawDirty] = useState(false);

  // --- simulation ----------------------------------------------------------
  const [motors, setMotors] = useState(() => ({ [SEED_ADDRESS]: createSeedMotor() }));
  const [manualValues, setManualValues] = useState(() => ({ ...MANUAL_DEFAULTS }));

  // --- trace ---------------------------------------------------------------
  const [records, setRecords] = useState(() => createSeedRecords(APP_START));
  const [traceFilter, setTraceFilter] = useState('all');
  const [traceQuery, setTraceQuery] = useState('');
  const [paused, setPaused] = useState(false);
  const [frozenAt, setFrozenAt] = useState(null);

  const [toast, setToast] = useState(null);

  // --- refs: live simulation bookkeeping ----------------------------------
  const motorsRef = useRef(motors);
  motorsRef.current = motors;
  const epochs = useRef(new Map());
  const timers = useRef(new Map());
  const motions = useRef(new Map());
  const feedbackLoops = useRef(new Map());
  const recordSeq = useRef(0);
  const toastTimer = useRef(null);

  const pushToast = useCallback((text, tone = 'ok') => {
    setToast({ text, tone, id: `${Date.now()}-${Math.random()}` });
    if (toastTimer.current) clearTimeout(toastTimer.current);
    toastTimer.current = setTimeout(() => setToast(null), 2400);
  }, []);

  const commitMotor = useCallback((addr, next) => {
    motorsRef.current = { ...motorsRef.current, [addr]: next };
    setMotors(motorsRef.current);
  }, []);

  const clearTimers = useCallback((addr) => {
    const set = timers.current.get(addr);
    if (!set) return;
    set.forEach((id) => clearTimeout(id));
    set.clear();
  }, []);

  const stopFeedbackLoop = useCallback((addr) => {
    const loop = feedbackLoops.current.get(addr);
    if (!loop) return;
    loop.stopped = true;
    if (loop.timerId) clearTimeout(loop.timerId);
    feedbackLoops.current.delete(addr);
  }, []);

  /** Cancels every pending simulated task for one motor address. */
  const bumpEpoch = useCallback((addr) => {
    const next = (epochs.current.get(addr) ?? 0) + 1;
    epochs.current.set(addr, next);
    clearTimers(addr);
    motions.current.delete(addr);
    return next;
  }, [clearTimers]);

  const scheduleTask = useCallback((addr, epoch, delay, run) => {
    const id = setTimeout(() => {
      const set = timers.current.get(addr);
      if (set) set.delete(id);
      if ((epochs.current.get(addr) ?? 0) !== epoch) return; // stale task: dropped
      run();
    }, delay);
    if (!timers.current.has(addr)) timers.current.set(addr, new Set());
    timers.current.get(addr).add(id);
  }, []);

  const appendRecords = useCallback((rows) => {
    if (rows.length === 0) return;
    setRecords((prev) => {
      const next = [...prev, ...rows];
      return next.length > MAX_RECORDS ? next.slice(next.length - MAX_RECORDS) : next;
    });
  }, []);

  const makeRecord = useCallback(({ at, dir, canId, dlc, data, note, addr }) => {
    recordSeq.current += 1;
    return {
      id: `r${recordSeq.current}`,
      at,
      dir,
      canId,
      dlc,
      data,
      note,
      address: addr,
      simulated: true,
    };
  }, []);

  const appendRx = useCallback((addr, data, at = Date.now()) => {
    const decoded = decodeFeedbackFrame(data);
    const note = decoded ? decoded.text : data[2] === 0x6b && data.length === 3 ? classifyAck(data[1]) : '模拟应答';
    appendRecords([
      makeRecord({ at, dir: 'RX', canId: addr << 8, dlc: data.length, data, note, addr }),
    ]);
    return { data, note };
  }, [appendRecords, makeRecord]);

  // --- motion engine -------------------------------------------------------
  const startMotion = useCallback((addr, plan, epoch, motor, opcode) => {
    const motion = plan.motion;
    if (!motion) return;
    const direction = motion.mode === 'continuous'
      ? (motion.direction ?? 1)
      : (motion.deltaTenths < 0 ? -1 : 1);
    motions.current.set(addr, {
      epoch,
      opcode,
      mode: motion.mode,
      startedAt: performance.now(),
      durationMs: motion.durationMs,
      fromTenths: motor.positionTenths,
      toTenths: motion.mode === 'bounded' ? motor.positionTenths + motion.deltaTenths : null,
      speedTenths: motion.speedTenths ?? 0,
      direction,
      currentMa: motion.currentMa || motor.currentMa,
    });
  }, []);

  useEffect(() => {
    const id = setInterval(() => {
      if (motions.current.size === 0) return;
      const now = performance.now();
      const nextMotors = { ...motorsRef.current };
      const completed = [];
      let mutated = false;

      for (const [key, motion] of [...motions.current.entries()]) {
        const motor = nextMotors[key];
        if (!motor || (epochs.current.get(key) ?? 0) !== motion.epoch) {
          motions.current.delete(key);
          continue;
        }
        const elapsed = now - motion.startedAt;
        const progress = Math.min(1, elapsed / motion.durationMs);
        const travelled = motion.direction * motion.speedTenths * 6 * (elapsed / 1000);

        if (progress >= 1) {
          const finalTenths = motion.mode === 'bounded'
            ? motion.toTenths
            : motor.positionTenths + motion.direction * motion.speedTenths * 6 * (motion.durationMs / 1000);
          nextMotors[key] = {
            ...motor,
            positionTenths: Math.round(finalTenths),
            velocityTenths: 0,
            currentMa: motion.currentMa || motor.currentMa,
          };
          motions.current.delete(key);
          completed.push({ addr: key, motion });
        } else {
          const eased = progress * progress * (3 - 2 * progress);
          const position = motion.mode === 'bounded'
            ? motion.fromTenths + (motion.toTenths - motion.fromTenths) * eased
            : motion.fromTenths + travelled;
          nextMotors[key] = {
            ...motor,
            positionTenths: Math.round(position),
            velocityTenths: motion.direction * motion.speedTenths,
            currentMa: motion.currentMa || motor.currentMa,
          };
        }
        mutated = true;
      }

      if (mutated) {
        motorsRef.current = nextMotors;
        setMotors(nextMotors);
      }

      for (const entry of completed) {
        const settled = motorsRef.current[entry.addr];
        if (!settled) continue;
        const positionFrame = encodePositionFrame(settled.positionTenths);
        appendRx(entry.addr, positionFrame);
        const done = ackFrame(entry.motion.opcode, ACK_STATUS.completed);
        appendRx(entry.addr, done);
        commitMotor(entry.addr, {
          ...settled,
          lastResponse: {
            data: positionFrame,
            kind: 'feedback',
            text: '运动完成回执',
            canId: entry.addr << 8,
            simulated: true,
          },
          status: { kind: 'feedback', text: '已收到 0x9F 完成回执（模拟）' },
        });
      }
    }, MOTION_TICK_MS);

    return () => clearInterval(id);
  }, [appendRx, commitMotor]);

  useEffect(() => () => {
    timers.current.forEach((set) => set.forEach((id) => clearTimeout(id)));
    feedbackLoops.current.forEach((loop) => {
      loop.stopped = true;
      if (loop.timerId) clearTimeout(loop.timerId);
    });
    if (toastTimer.current) clearTimeout(toastTimer.current);
  }, []);

  const startFeedbackLoop = useCallback((addr, intervalMs) => {
    stopFeedbackLoop(addr);
    if (!intervalMs) return;
    const period = Math.max(FEEDBACK_DEMO_MIN_INTERVAL_MS, intervalMs);
    const loop = { stopped: false, count: 0, timerId: null };
    const step = () => {
      const motor = motorsRef.current[addr];
      if (!motor || loop.stopped || loop.count >= FEEDBACK_DEMO_MAX_FRAMES) {
        stopFeedbackLoop(addr);
        return;
      }
      loop.count += 1;
      appendRx(addr, encodePositionFrame(motor.positionTenths));
      loop.timerId = setTimeout(step, period);
    };
    loop.timerId = setTimeout(step, period);
    feedbackLoops.current.set(addr, loop);
  }, [appendRx, stopFeedbackLoop]);

  // --- derived command model ----------------------------------------------
  const item = getCommandItem(selectedId) ?? getCommandItem('enable');
  const variant = getVariant(item, variantKey);
  const motor = motors[address] ?? createMotor(address);

  const formEncode = useMemo(
    () => encodeCommand({ item, variantKey, values, address }),
    [item, variantKey, values, address],
  );

  const rawParse = useMemo(() => parseLogicalHex(rawText), [rawText]);
  const rawDisplay = rawDirty ? rawText : (formEncode.ok ? formatBytes(formEncode.bytes) : '');

  /**
   * Hand-built bytes (raw editor or the custom frame form) go through the same
   * gate: a known function code must decode into a valid command, otherwise the
   * frame is rejected instead of being passed off as an unknown frame.
   */
  const resolveHandBuiltBytes = useCallback((bytes) => {
    const reject = (message) => ({
      ok: false,
      errors: { _raw: [message] },
      bytes: [],
      labels: [],
      item: null,
      variantKey: 'base',
      values: {},
    });

    const opcode = bytes[1];
    const match = identifyCommandBytes(bytes, address, item.id);

    if (!match) {
      const knownOpcode = COMMAND_ITEMS.some(
        (entry) => !entry.custom && entry.variants.some((entryVariant) => entryVariant.opcode === opcode),
      );
      if (knownOpcode) {
        return reject(`功能码 0x${hexByte(opcode)} 是已知指令，但字节结构不匹配（常量字节或长度不正确），已拒绝发送。`);
      }
      return {
        ok: true,
        errors: {},
        bytes,
        labels: RAW_FALLBACK_LABELS(bytes),
        item: getCommandItem('customFrame'),
        variantKey: 'base',
        values: {},
        identified: false,
      };
    }

    const rebuilt = encodeCommand({
      item: match.item,
      variantKey: match.variant.key,
      values: match.values,
      address,
    });
    if (!rebuilt.ok || formatBytes(rebuilt.bytes) !== formatBytes(bytes)) {
      return reject(`已匹配到「${match.item.name}」，但参数未通过校验：${
        Object.values(rebuilt.errors ?? {}).join('；') || '字节结构与编码结果不一致'
      }`);
    }
    return {
      ok: true,
      errors: {},
      bytes,
      labels: rebuilt.labels,
      item: match.item,
      variantKey: match.variant.key,
      values: match.values,
      identified: true,
    };
  }, [address, item.id]);

  const model = useMemo(() => {
    if (editorMode === 'raw' && rawDirty) {
      if (!rawParse.ok) {
        return {
          ok: false,
          errors: { _raw: rawParse.errors },
          bytes: [],
          labels: [],
          item: null,
          variantKey: 'base',
          values: {},
        };
      }
      // The logical command carries the address, but the CAN identifier is built
      // from the toolbar setting: they must not diverge.
      if (rawParse.bytes[0] !== address) {
        return {
          ok: false,
          errors: {
            _raw: [
              `原始指令首字节是电机地址，必须与顶部 CAN ID 一致（当前设置 0x${hexByte(address)}，指令里是 0x${hexByte(rawParse.bytes[0])}）`,
            ],
          },
          bytes: [],
          labels: [],
          item: null,
          variantKey: 'base',
          values: {},
        };
      }
      return resolveHandBuiltBytes(rawParse.bytes);
    }

    if (item.custom) {
      if (!formEncode.ok) {
        return {
          ok: false,
          errors: { ...formEncode.errors },
          bytes: [],
          labels: [],
          item,
          variantKey,
          values,
        };
      }
      return resolveHandBuiltBytes(formEncode.bytes);
    }

    return {
      ok: formEncode.ok,
      errors: formEncode.ok ? {} : { ...formEncode.errors },
      bytes: formEncode.bytes ?? [],
      labels: formEncode.labels ?? [],
      item,
      variantKey,
      values,
    };
  }, [editorMode, rawDirty, rawParse, formEncode, item, variantKey, values, address, resolveHandBuiltBytes]);

  const frames = useMemo(() => buildFrames(model.bytes), [model.bytes]);
  const annotations = useMemo(
    () => frames.map((frame) => frameAnnotation(model.bytes, model.labels, frame)),
    [frames, model.bytes, model.labels],
  );

  const addressCheck = validateAddress(addressDraft);
  const addressError = addressCheck.ok ? null : addressCheck.error;

  // --- sending -------------------------------------------------------------
  const applyPlan = useCallback((addr, plan, ctx, epoch, { fromQueue = false } = {}) => {
    const current = motorsRef.current[addr] ?? createMotor(addr);
    let next = { ...current };
    let status = { kind: 'queued', text: '已发送，等待应答' };

    // A mutating command always invalidates the running simulation: the motion
    // descriptor is already gone (the caller bumped the epoch) and the stale
    // speed/current readouts are cleared here.
    if (plan.cancelsMotion && (!plan.sync || fromQueue)) {
      next.velocityTenths = 0;
      next.currentMa = 0;
    }

    const queue = (entry) => {
      next.syncQueue = [...current.syncQueue, entry];
      status = { kind: 'queued', text: `已排队（${next.syncQueue.length} 条待触发）` };
    };

    switch (plan.kind) {
      case 'enable':
      case 'disable':
        if (plan.sync && !fromQueue) {
          queue({ plan, ctx, label: ctx.item.name });
        } else {
          next.enabled = plan.enabled;
          // Disabling drops anything that was waiting for a sync trigger.
          if (!plan.enabled) next.syncQueue = [];
          if (fromQueue) status = { kind: 'received', text: '同步触发已执行（模拟）' };
        }
        break;

      case 'stop':
        if (plan.sync && !fromQueue) {
          queue({ plan, ctx, label: ctx.item.name });
        } else {
          next.syncQueue = [];
          status = { kind: 'queued', text: '已发送停止，等待应答' };
        }
        break;

      case 'sync-trigger': {
        const pending = current.syncQueue ?? [];
        if (pending.length === 0) {
          status = { kind: 'received', text: '已接收指令（无待触发项）' };
        } else {
          let offset = 0;
          pending.forEach((entry) => {
            scheduleTask(addr, epoch, 20 + offset, () => {
              if (!motorsRef.current[addr]?.enabled && entry.plan.kind === 'motion') {
                commitMotor(addr, {
                  ...motorsRef.current[addr],
                  status: { kind: 'error', text: '队列执行时电机未使能：已跳过（模拟）' },
                });
                return;
              }
              applyPlan(addr, entry.plan, entry.ctx, epoch, { fromQueue: true });
            });
            offset += (entry.plan.motion?.durationMs ?? 40) + 60;
          });
          next.syncQueue = [];
          status = { kind: 'queued', text: `已触发 ${pending.length} 条排队指令（模拟）` };
        }
        break;
      }

      case 'motion':
        if (!plan.motion) {
          status = { kind: 'received', text: '已发送（速度为 0，无位移）' };
        } else if (plan.sync && !fromQueue) {
          queue({ plan, ctx, label: ctx.item.name });
        } else if (!current.enabled) {
          // Queued work is re-checked: a disable after queueing must not move.
          status = { kind: 'error', text: '电机未使能：已跳过模拟运动' };
        } else {
          startMotion(addr, plan, epoch, current, ctx.opcode);
          next.currentMa = plan.motion.currentMa || current.currentMa;
          next.velocityTenths = plan.motion.mode === 'continuous'
            ? (plan.motion.direction ?? 1) * plan.motion.speedTenths
            : 0;
          status = {
            kind: 'queued',
            text: fromQueue ? '同步触发已执行（模拟运动）' : '运动已开始（模拟）',
          };
        }
        break;

      case 'torque':
        if (plan.sync && !fromQueue) {
          queue({ plan, ctx, label: ctx.item.name });
        } else {
          next.currentMa = plan.currentMa;
          status = { kind: 'received', text: '电流已按模拟施加' };
        }
        break;

      case 'not-modelled':
        status = { kind: 'unsupported', text: '未建模：仅发送帧' };
        next.lastResponse = null;
        break;

      case 'zero-position':
        next.positionTenths = 0;
        next.velocityTenths = 0;
        break;

      case 'config':
        if (plan.ctrlMode !== undefined) next.ctrlMode = plan.ctrlMode;
        if (plan.feedbackIntervalMs !== undefined) {
          next.feedbackIntervalMs = plan.feedbackIntervalMs;
          startFeedbackLoop(addr, plan.feedbackIntervalMs);
        }
        break;

      case 'read':
        break;

      case 'device-note':
        break;

      case 'unsupported':
        commitMotor(addr, { ...next, lastResponse: null, status: { kind: 'unsupported', text: '无解码器 / 模拟不可用' } });
        return { plan, simulated: false };

      default:
        break;
    }

    next.status = status;
    commitMotor(addr, next);

    if (plan.ackStatus === null || plan.ackStatus === undefined) return { plan, simulated: false };

    const opcode = ctx.opcode;
    scheduleTask(addr, epoch, ACK_DELAY_MS, () => {
      const data = ackFrame(opcode, plan.ackStatus);
      const text = classifyAck(plan.ackStatus);
      appendRx(addr, data);
      const settled = motorsRef.current[addr];
      if (!settled) return;
      commitMotor(addr, {
        ...settled,
        lastResponse: { data, kind: 'ack', text, canId: addr << 8, simulated: true },
        status: plan.kind === 'motion' && plan.motion
          ? { kind: 'received', text: '已接收指令（等待运动反馈）' }
          : { kind: 'received', text },
      });
    });

    if (plan.kind === 'read' && plan.decoder) {
      scheduleTask(addr, epoch, FEEDBACK_DELAY_MS, () => {
        const settled = motorsRef.current[addr];
        if (!settled) return;
        const data = feedbackFrameFor(plan.decoder, settled);
        if (!data) return;
        appendRx(addr, data);
        commitMotor(addr, {
          ...settled,
          lastResponse: { data, kind: 'feedback', text: '模拟数据帧', canId: addr << 8, simulated: true },
          status: { kind: 'feedback', text: '已返回模拟数据帧' },
        });
      });
    }

    return { plan, simulated: true };
  }, [appendRx, commitMotor, scheduleTask, startFeedbackLoop, startMotion]);

  const commitSend = useCallback(({ sendItem, sendVariantKey, sendValues, bytes }) => {
    const addr = address;
    const sendFrames = buildFrames(bytes);
    const startedAt = Date.now();
    appendRecords(sendFrames.map((frame, index) => makeRecord({
      at: startedAt + index * 2,
      dir: 'TX',
      canId: frame.canId,
      dlc: frame.dlc,
      data: frame.data,
      note: sendFrames.length > 1
        ? `${sendItem.sendNote} · Packet ${index}/${sendFrames.length}`
        : sendItem.sendNote,
      addr,
    })));

    const plan = planCommandEffect({ item: sendItem, variantKey: sendVariantKey, values: sendValues });
    // Read/config commands must not abort a running motion, so only commands
    // that mutate the motor invalidate the pending simulation.
    const epoch = plan.cancelsMotion && !plan.sync ? bumpEpoch(addr) : (epochs.current.get(addr) ?? 0);
    const current = motorsRef.current[addr] ?? createMotor(addr);
    const ctx = { item: sendItem, variantKey: sendVariantKey, values: sendValues, opcode: bytes[1] };
    const result = applyPlan(addr, plan, ctx, epoch);

    if (!result.simulated) {
      pushToast(plan.note, plan.kind === 'not-modelled' ? 'info' : 'warn');
    } else if (plan.kind === 'device-note') {
      pushToast('该指令网页未接入：仅发送真实帧并记录模拟说明', 'info');
    }
    return { plan, epoch, current };
  }, [address, appendRecords, applyPlan, bumpEpoch, makeRecord, pushToast]);

  const handleSend = useCallback(() => {
    const sendItem = model.item;
    if (!sendItem) {
      pushToast('逻辑指令无效，无法发送', 'warn');
      return;
    }
    commitSend({
      sendItem,
      sendVariantKey: model.variantKey,
      sendValues: model.values,
      bytes: model.bytes,
    });
  }, [commitSend, model, pushToast]);

  const handleStopAll = useCallback(() => {
    const known = new Set([
      ...[...motions.current.keys()].map(Number),
      ...[...timers.current.keys()].map(Number),
      ...[...feedbackLoops.current.keys()].map(Number),
      ...Object.keys(motorsRef.current).map(Number),
      address,
    ]);
    // 1. cancel every pending simulated task and queued sync entry
    known.forEach((addr) => {
      stopFeedbackLoop(addr);
      bumpEpoch(addr);
    });
    const next = { ...motorsRef.current };
    for (const key of Object.keys(next)) {
      next[key] = {
        ...next[key],
        velocityTenths: 0,
        currentMa: 0,
        syncQueue: [],
        status: { kind: 'received', text: '已接收广播停止（模拟）' },
      };
    }
    motorsRef.current = next;
    setMotors(next);

    // 2. emit the real broadcast stop frame: identifier 0, FE 98 00 6B.
    //    A broadcast has no addressed responder, so no ACK is simulated.
    appendRecords([
      makeRecord({
        at: Date.now(),
        dir: 'TX',
        canId: BROADCAST_CAN_ID,
        dlc: 4,
        data: [0xfe, 0x98, 0x00, PROTOCOL_CHECKSUM],
        note: '广播停止请求（ID 0x00000000）',
        addr: 0,
      }),
    ]);
    pushToast('已发送广播停止（ID 0x00000000）：本地模拟任务与队列已清空，未伪造广播回执', 'ok');
  }, [address, appendRecords, bumpEpoch, makeRecord, pushToast, stopFeedbackLoop]);

  const handleManualStop = useCallback(() => {
    const addr = address;
    stopFeedbackLoop(addr);
    bumpEpoch(addr);
    const current = motorsRef.current[addr] ?? createMotor(addr);
    commitMotor(addr, {
      ...current,
      velocityTenths: 0,
      currentMa: 0,
      syncQueue: [],
      status: { kind: 'received', text: '已停止（模拟）' },
    });
    if (connected) {
      const stopItem = getCommandItem('stop');
      const stopValues = defaultValues(stopItem, 'base');
      const encoded = encodeCommand({ item: stopItem, variantKey: 'base', values: stopValues, address: addr });
      if (encoded.ok) {
        commitSend({
          sendItem: stopItem,
          sendVariantKey: 'base',
          sendValues: stopValues,
          bytes: encoded.bytes,
        });
      }
    }
    pushToast('已停止当前电机的模拟运动', 'ok');
  }, [address, bumpEpoch, commitMotor, commitSend, connected, pushToast, stopFeedbackLoop]);


  // --- copy / toast --------------------------------------------------------
  const fallbackCopy = (text) => {
    const area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', 'readonly');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    const ok = document.execCommand('copy');
    document.body.removeChild(area);
    if (!ok) throw new Error('copy failed');
  };

  const handleCopy = useCallback(async (text, label = '内容') => {
    const value = String(text ?? '');
    if (!value || value === '—') {
      pushToast('没有可复制的内容', 'warn');
      return;
    }
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(value);
      else fallbackCopy(value);
      pushToast(`已复制${label}：${value.length > 32 ? `${value.slice(0, 32)}…` : value}`, 'ok');
    } catch {
      try {
        fallbackCopy(value);
        pushToast(`已复制${label}（兼容模式）`, 'ok');
      } catch {
        pushToast('复制失败：当前环境不允许访问剪贴板', 'warn');
      }
    }
  }, [pushToast]);

  // --- handlers ------------------------------------------------------------
  const handleAddressChange = useCallback((text) => {
    setAddressDraft(text);
    const check = validateAddress(text);
    if (!check.ok) return;
    if (check.value === address) return;

    // Switching motors cancels the previous motor's pending work: no delayed
    // completion may land on a motor the operator is no longer looking at.
    const previous = motorsRef.current[address];
    if (previous) {
      stopFeedbackLoop(address);
      bumpEpoch(address);
      commitMotor(address, {
        ...previous,
        velocityTenths: 0,
        currentMa: 0,
        syncQueue: [],
        status: { kind: 'idle', text: '已切换电机：模拟任务已停止' },
      });
    }

    setAddress(check.value);
    const existing = motorsRef.current[check.value];
    pushToast(
      `已切换到电机 0x${hexByte(check.value)}：${
        existing ? '沿用该地址已有的独立模拟状态' : '该地址从零开始（未使能，位置 0.0°，独立状态）'
      }`,
      'info',
    );
  }, [address, bumpEpoch, commitMotor, pushToast, stopFeedbackLoop]);

  const handleSelect = useCallback((id) => {
    const next = getCommandItem(id);
    if (!next) return;
    setSelectedId(id);
    setVariantKey(next.variants[0].key);
    setValues(defaultValues(next, next.variants[0].key));
    setRawDirty(false);
    setRawText('');
    setEditorMode('form');
  }, []);

  const handleVariantChange = useCallback((key) => {
    const nextFields = getVariant(item, key)?.layout.filter((segment) => segment.kind === 'field') ?? [];
    const carried = {};
    for (const field of nextFields) {
      if (values[field.key] !== undefined) carried[field.key] = values[field.key];
    }
    setVariantKey(key);
    setValues({ ...defaultValues(item, key), ...carried });
    setRawDirty(false);
  }, [item, values]);

  const handleValueChange = useCallback((key, value) => {
    setValues((prev) => ({ ...prev, [key]: value }));
  }, []);

  const handleEditorModeChange = useCallback((mode) => {
    if (mode === 'raw' && !rawDirty) {
      setRawText(formEncode.ok ? formatBytes(formEncode.bytes) : '');
    }
    setEditorMode(mode);
  }, [formEncode, rawDirty]);

  const handleRawTextChange = useCallback((text) => {
    setRawText(text);
    setRawDirty(true);
  }, []);

  const handleResetRaw = useCallback(() => {
    setRawDirty(false);
    setRawText('');
  }, []);

  const toggleGroup = useCallback((groupId) => {
    setOpenGroups((prev) => ({ ...prev, [groupId]: !prev[groupId] }));
  }, []);

  const togglePause = useCallback(() => {
    if (paused) {
      setPaused(false);
      setFrozenAt(null);
    } else {
      setPaused(true);
      setFrozenAt(records.length);
    }
  }, [paused, records.length]);

  const handleClear = useCallback(() => {
    setRecords([]);
    if (paused) setFrozenAt(0);
    pushToast('收发记录已清空', 'ok');
  }, [paused, pushToast]);

  // --- manual tab ----------------------------------------------------------
  const manualPlan = useMemo(() => planManualMove(manualValues), [manualValues]);
  const manualEncode = useMemo(() => {
    if (!manualPlan.ok) return { bytes: [], frames: [], annotations: [] };
    const encoded = encodeCommand({
      item: getCommandItem('position'),
      variantKey: 'limit',
      values: manualPlan.plan,
      address,
    });
    if (!encoded.ok) return { bytes: [], frames: [], annotations: [] };
    const built = buildFrames(encoded.bytes);
    return {
      bytes: encoded.bytes,
      frames: built,
      annotations: built.map((frame) => frameAnnotation(encoded.bytes, encoded.labels, frame)),
    };
  }, [manualPlan, address]);

  const manualGate = useMemo(() => {
    if (!connected) return '模拟连接已断开：请点击右上角恢复连接。';
    if (addressError) return `CAN ID 无效：${addressError}`;
    if (!manualPlan.ok) {
      const first = Object.values(manualPlan.errors)[0];
      return `无法发送：${first}`;
    }
    if (!motor.enabled) return `电机 0x${hexByte(address)} 未使能：请先发送 F3 使能指令（模拟门限）。`;
    return null;
  }, [address, addressError, connected, manualPlan, motor.enabled]);

  const handleManualSend = useCallback(() => {
    if (manualGate) {
      pushToast(manualGate, 'warn');
      return;
    }
    commitSend({
      sendItem: getCommandItem('position'),
      sendVariantKey: 'limit',
      sendValues: manualPlan.plan,
      bytes: manualEncode.bytes,
    });
  }, [commitSend, manualEncode.bytes, manualGate, manualPlan.plan, pushToast]);

  // --- gates for the lab ---------------------------------------------------
  const needsEnabled = Boolean(
    model.item && ['torque', 'velocity', 'position', 'passthroughPosition'].includes(model.item.id),
  );

  const gateReason = useMemo(() => {
    if (!connected) return '模拟连接已断开：请点击右上角“未连接”恢复后再发送。';
    if (addressError) return `CAN ID 无效（${addressError}），修正后才能发送。`;
    if (!model.ok) {
      const first = Object.values(model.errors).flat().find(Boolean);
      return first ? `无法发送：${first}` : '无法发送：请先修正参数。';
    }
    if (needsEnabled && !motor.enabled) {
      return `电机 0x${hexByte(address)} 未使能：运动类指令会被拒绝，请先发送 F3 使能指令。`;
    }
    return null;
  }, [address, addressError, connected, model, motor.enabled, needsEnabled]);

  const visibleRecords = useMemo(() => {
    const base = paused ? records.slice(0, frozenAt ?? records.length) : records;
    const needle = traceQuery.trim().toLowerCase();
    return base.filter((record) => {
      if (traceFilter !== 'all' && record.dir !== traceFilter) return false;
      if (!needle) return true;
      const haystack = `${record.dir} ${formatCanId(record.canId)} ${record.dlc} ${formatBytes(record.data)} ${record.note}`.toLowerCase();
      return haystack.includes(needle);
    });
  }, [records, paused, frozenAt, traceFilter, traceQuery]);

  return (
    <div className="app">
      <Toolbar
        addressDraft={addressDraft}
        addressError={addressError}
        addressValue={address}
        onAddressChange={handleAddressChange}
        bitrate={bitrate}
        onBitrateChange={setBitrate}
        connected={connected}
        onToggleConnection={() => {
          const next = !connected;
          setConnected(next);
          pushToast(next ? '已恢复模拟连接（仍不访问真实硬件）' : '已断开模拟连接：发送被禁用', 'info');
        }}
        onStopAll={handleStopAll}
      />

      <nav className="tabs" role="tablist" aria-label="工作模式">
        {TABS.map((entry) => (
          <button
            type="button"
            key={entry.id}
            role="tab"
            aria-selected={tab === entry.id}
            className={`tabs__item${tab === entry.id ? ' is-active' : ''}`}
            onClick={() => setTab(entry.id)}
          >
            {entry.label}
          </button>
        ))}
      </nav>

      <main className={`workspace workspace--${tab}`}>
        {tab === 'lab' ? (
          <>
            <CommandLibrary
              query={query}
              onQueryChange={setQuery}
              openGroups={openGroups}
              onToggleGroup={toggleGroup}
              selectedId={selectedId}
              onSelect={handleSelect}
            />
            <CommandPanel
              item={item}
              variantKey={variant.key}
              onVariantChange={handleVariantChange}
              values={values}
              onValueChange={handleValueChange}
              editorMode={editorMode}
              onEditorModeChange={handleEditorModeChange}
              rawText={rawDisplay}
              onRawTextChange={handleRawTextChange}
              onRawTextReplace={setRawText}
              rawDirty={rawDirty}
              onResetRaw={handleResetRaw}
              identifiedItem={model.identified ? model.item : null}
              model={model}
              frames={frames}
              annotations={annotations}
              address={address}
              addressError={addressError}
              gateReason={gateReason}
              onSend={handleSend}
              onCopy={handleCopy}
            />
            <FeedbackPanel motor={motor} address={address} pendingCount={motor.syncQueue?.length ?? 0} />
          </>
        ) : (
          <>
            <FeedbackPanel motor={motor} address={address} pendingCount={motor.syncQueue?.length ?? 0} />
            <ManualPanel
              values={manualValues}
              errors={manualPlan.errors}
              onChange={(key, value) => setManualValues((prev) => ({ ...prev, [key]: value }))}
              motor={motor}
              address={address}
              prediction={manualPlan}
              bytes={manualEncode.bytes}
              frames={manualEncode.frames}
              annotations={manualEncode.annotations}
              gateReason={manualGate}
              onSend={handleManualSend}
              onStop={handleManualStop}
              onCopy={handleCopy}
              onGoToEnable={() => {
                setTab('lab');
                handleSelect('enable');
              }}
            />
          </>
        )}
      </main>

      <TracePanel
        records={visibleRecords}
        totalCount={records.length}
        hiddenCount={paused ? records.length - (frozenAt ?? records.length) : 0}
        filter={traceFilter}
        onFilterChange={setTraceFilter}
        query={traceQuery}
        onQueryChange={setTraceQuery}
        paused={paused}
        onTogglePause={togglePause}
        onClear={handleClear}
        onCopy={handleCopy}
      />

      {toast ? (
        <div className={`toast toast--${toast.tone}`} role="status" key={toast.id}>
          {toast.text}
        </div>
      ) : null}
    </div>
  );
}
