import { useEffect, useRef, useState } from 'react';
import { CommandLibrary } from './components/CommandLibrary.jsx';
import { CommandPanel } from './components/CommandPanel.jsx';
import { LimitsPanel } from './components/LimitsPanel.jsx';
import { ManualPanel } from './components/ManualPanel.jsx';
import { QueuePanel } from './components/QueuePanel.jsx';
import { DemoPanel } from './components/DemoPanel.jsx';
import { ConfigResult } from './components/ConfigResult.jsx';
import { DebugLogPanel } from './components/DebugLogPanel.jsx';
import { logStore } from './debug-log.js';
import { queueActionLabels, queueMessageText } from './device-api.js';
import { TracePanel } from './components/TracePanel.jsx';
import { WifiPanel } from './components/WifiPanel.jsx';
import { ScaleWorkbench } from './components/ScaleWorkbench.jsx';
import { GlyphInfo } from './components/glyphs.jsx';
import { COMMAND_GROUPS, getCommandItem, getFields, encodeCommand, buildFrames, frameAnnotation, formatBytes, formatCanId, hexByte, parseLogicalHex, identifyCommandBytes, validateAddress } from './protocol.js';
import { planManualMove, MANUAL_DEFAULTS } from './simulation.js';
import { decodeCanReply, decodeBulkCanReply } from './manual-reference.js';
import { deviceDefaults, experimentWindowText, limitsEqual, limitsToManualLimits, readLimitsPayload } from './device-limits.js';
import { readDrafts, writeDrafts, safeStorage, getForm, putForm, lastVariantFor, getManual, putManual, putSelection, pickFields } from './device-drafts.js';
import { request, supportReason, directPositionBoardNote, isMotionOpcode, isLimitDependentOpcode, stateLabels, errorLabels, readQueueStatus, queueProgressText, homeStatusText, queueConflictReason } from './device-api.js';

const EMPTY = {id:0, enabled:false, online:false, positionDeg:null, speedRpm:null, currentMa:null};

// 常规试动 fields: drafts are kept per motor, and the values stay exactly as the
// user typed them (including partial or invalid text).
const MANUAL_KEYS = ['dir', 'angle', 'speed', 'accel', 'decel', 'current'];
const MANUAL_FIELDS = {...MANUAL_DEFAULTS, angle:'10', speed:'10', current:'300'};

// One short, visible reminder: these inputs are local drafts, not read-backs of
// the board's confirmed parameters.
const DRAFT_NOTE = '表单与 HEX 内容都是本机草稿：切换电机／指令、切换标签页或刷新页面都会保留，也永远不会被自动发送；它们不是板端已确认的参数、使能或执行状态。';

// Board-side ownership reasons (statusJson control.blockers[].reason), shown next
// to the motor id. Unknown reasons are printed as they arrive, never guessed at.
const blockerLabels = {
  config_pending:'配置核对中', move_active:'移动进行中', home_active:'回零进行中',
  experiment_active:'试验进行中', enable_pending:'等待使能应答', stop_pending:'等待停止反馈',
};

const fieldKeys = (item, variantKey) => getFields(item, variantKey).map(field => field.key);
const variantKeyOf = (item, key) =>
  item?.variants?.some(variant => variant.key === key) ? key : (item?.variants?.[0]?.key ?? 'base');

// The context a reload comes back to: the last valid motor, command and variant,
// with whatever the user had typed for them. Values are never sent anywhere.
function bootContext(drafts) {
  const motorCheck = validateAddress(drafts.lastMotorId);
  const motorText = motorCheck.ok ? String(motorCheck.value) : '1';
  const motorId = String(validateAddress(motorText).value);
  const item = getCommandItem(drafts.lastCommandId) ?? getCommandItem('enable');
  const variantKey = variantKeyOf(item, drafts.lastVariantKey);
  const stored = getForm(drafts, motorId, item.id, variantKey);
  // Limits are not confirmed yet at boot, so defaults are not clamped here; a
  // restored draft is never clamped at all.
  const base = deviceDefaults(item, variantKey, null);
  return {
    motorText, motorId, commandId: item.id, variantKey,
    values: stored?.values ? {...base, ...pickFields(stored.values, fieldKeys(item, variantKey))} : base,
    mode: stored?.mode === 'raw' ? 'raw' : 'form',
    raw: stored?.raw ?? '',
    dirty: !!stored?.dirty,
    manual: {...MANUAL_FIELDS, ...pickFields(getManual(drafts, motorId) ?? {}, MANUAL_KEYS)},
  };
}

// Catalog notes are written for the offline simulator and promise local
// simulation. The device page keeps only the manual-grounded sentences and
// drops anything that describes simulated behaviour; byte layouts, field keys
// and parameter help stay exactly as they are.
const SIMULATED_SENTENCE = /(模拟|演示|仿真|网页侧)/;
const deviceNote = text => String(text || '')
  .split('。')
  .map(part => part.trim())
  .filter(part => part && !SIMULATED_SENTENCE.test(part))
  .map(part => `${part}。`)
  .join('');
const cleanItem = original => ({
  ...original,
  note: original.id === 'passthroughPosition'
    ? 'FB／CB 已接入板端位置监督，支持三种立即执行模式。相对上一目标需要驱动器的新鲜目标反馈；实际行程由板端校验。FB 不携带电流限制，使用驱动器自身设置；需要指定电流上限请选择 CB。'
    : deviceNote(original.note) || null,
  variants: original.variants.map(v => ({...v,layout:v.layout.map(f => f.key === 'sync'
    ? {...f,hint:'实机只接受立即执行；同步选项用于组帧预览。'}
    : f.key === 'motionMode' && original.id === 'passthroughPosition'
      ? {...f,hint:'00 相对驱动器上一输入目标；01 相对坐标零点的绝对位置；02 相对当前实际位置。方向决定位置数值的正负，三种模式均由板端监督实际到位。'}
      : f)})),
});
const metric = (value, unit) => value == null ? '—' : `${value} ${unit}`;
const oneLine = text => String(text || '').replace(/\s*\n+\s*/g, ' · ');
function formatClock(timestamp) {
  if (!Number.isFinite(timestamp)) return '—';
  const date = new Date(timestamp), pad = (value, width = 2) => String(value).padStart(width, '0');
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`;
}

function DeviceFeedback({ status, connected, live, notice, lab, address, opcode, records, experiment, limitsReady, queue, queueRunning, queueStale, directNote, draftNote }) {
  // Latest decoded reply for the address and function code currently selected.
  // Read-only history: it never feeds the gate, the metrics or the enable state.
  const query = lab && live && Number.isInteger(opcode)
    ? [...records].reverse().find(r => r.decoded && r.decoded.opcode === opcode && r.decoded.address === address)
    : null;
  // Supervised homing reports its own outcome; a 00 flag byte alone never means
  // "this run finished", so the wording stays with the board's outcome.
  const home = connected ? homeStatusText(status) : null;
  return <section className="panel panel--feedback" aria-label="电机反馈">
    <div className="feedback__head"><h2 className="panel__title">电机反馈</h2><span className={`chip ${status.enabled && connected ? 'chip--ok' : 'chip--muted'}`}>{connected ? status.enabled ? '使能已确认' : '使能未确认' : '反馈未知'}</span></div>
    <p className="feedback__address">电机 {status.id || '—'} · {connected && status.online ? '实时反馈' : '等待新鲜反馈'}</p>
    <dl className="metrics"><div className="metrics__row"><dt>位置</dt><dd>{metric(status.positionDeg,'°')}</dd></div><div className="metrics__row"><dt>速度</dt><dd>{metric(status.speedRpm,'RPM')}</dd></div><div className="metrics__row"><dt>电流</dt><dd>{metric(status.currentMa,'mA')}</dd></div></dl>
    {lab ? <>
      <div className="divider"/>
      <div className="feedback__head"><h3 className="section__title">查询结果</h3>{Number.isInteger(opcode) ? <span className="chip chip--code">0x{hexByte(opcode)}</span> : null}</div>
      {!live ? <p className="capabilities__note">设备未连接：查询结果已隐藏，避免显示其它地址的历史回包。</p>
        : !Number.isInteger(opcode) ? <p className="capabilities__note">当前报文没有可识别的功能码，暂无可查询的回包。</p>
        : query ? <>
          <p className="device-decoded__meta">最近收到 · 历史记录，非实时保证 · 本机接收时间 {formatClock(query.at)}</p>
          <p className="device-decoded__title">{query.decoded.title}（电机 {query.decoded.address}）</p>
          <p className="device-decoded__text">{query.decoded.text}</p>
          <p className="capabilities__note">只读历史回包：不改变使能、位置读数或发送许可。</p>
        </>
        : [0x1a,0x21].includes(opcode) ? <p className="device-notice">此查询的返回布局未确认，请在底部收发记录查看原始回包。</p>
        : [0x42,0x43].includes(opcode) ? <p className="device-notice">等待完整的五包参数回读；缺包时请在底部收发记录查看原始帧。</p>
        : <p className="device-notice">尚未收到此查询的有效回包</p>}
    </> : null}
    <div className="divider"/><h3 className="section__title">控制状态</h3>
    <p>{connected ? stateLabels[status.state] || status.state || '读取中' : '设备离线，读数已清空'}</p>
    {/* Fresh board-side ownership summary: who is holding the board, and why.
        Ages are not shown here on purpose - they change on every poll and say
        nothing the reason does not. */}
    {connected && status.control ? <p>
      板端占用：{status.control.busy ? '忙' : '空闲'}
      {Array.isArray(status.control.blockers) && status.control.blockers.length
        ? ` · ${status.control.blockers.slice(0, 4).map((entry) => `电机 ${entry.id}（${blockerLabels[entry.reason] || entry.reason}）`).join('、')}`
        : ''}
      {Number(status.control.blockerCount) > 4 ? ` · 等共 ${status.control.blockerCount} 项` : ''}
      {status.control.fault && status.control.fault !== 'none' ? ` · 故障 ${errorLabels[status.control.fault] || status.control.fault}` : ''}
    </p> : null}
    <p>CAN：{status.busState || '—'} · TX 错误 {status.txErrors ?? '—'}</p>
    <p>驱动实际使能：{status.driverEnabled == null ? '未知' : status.driverEnabled ? '开' : '关'}</p>
    <p>最近控制应答：{status.lastAck || '—'}</p>
    {status.lastMoveFailure && <details><summary>上次运动失败现场（历史）</summary>
      <p>目标 {metric(status.lastMoveFailure.targetDeg,'°')} · 实际 {metric(status.lastMoveFailure.positionDeg,'°')}</p>
      <p>速度 {metric(status.lastMoveFailure.speedRpm,'RPM')} · 已确认使能 {status.lastMoveFailure.enabled?'开':'关'}</p>
      <p>耗时 {status.lastMoveFailure.elapsedMs} ms / 期限 {status.lastMoveFailure.deadlineMs} ms</p>
    </details>}
    {home ? <p>回零：{home}</p> : null}
    {status.fault && status.fault !== 'none' && <p className="device-error" role="alert">{errorLabels[status.fault] || status.fault}</p>}
    {queue ? <>
      <div className="divider"/><h3 className="section__title">板端队列</h3>
      <p>{queueProgressText(queue)}{queueStale ? '（读取失败：以下为最后一次成功读取的板端状态）' : ''}</p>
      {queue.action ? <p>当前动作：{queueActionLabels[queue.action] || queue.action}{['hex','can'].includes(queue.action) ? '（原始帧，无运动监督）' : ''}</p> : null}
      {queue.message ? <p className="device-notice">{queueMessageText(queue.message)}</p> : null}
      <p className="capabilities__note">队列由板端独立执行：浏览器断开后，板端仍继续执行已提交的队列。本页只显示板端返回的状态。{queueRunning ? '运行期间常规试动与配置已锁定；「全部停止」会先取消队列。' : ''}</p>
    </> : null}
    <div className="divider"/><h3 className="section__title">请求结果</h3><p className="device-notice" role="status">{notice || '尚未提交操作'}</p>
    {draftNote ? <p className="capabilities__note">{draftNote}</p> : null}
    <p className="capabilities__note">只有收到 202 才表示指令已入队；超时或断线时请求结果未知，本页不会自动重发。使能及停止仍以真实应答／反馈确认，未知应答仅保留原始字节。</p>
    <div className="divider"/><h3 className="section__title">试验边界</h3><p className="capabilities__note">{experiment ? `速度／力矩试验：${experiment}；反馈超时提前停止。` : '速度／力矩试验的时长由板端策略决定（未读取到限制）。'}回零（9A）已接入板端监督：等待应答、3B 回零标志与新鲜静止反馈；直通位置（FB/CB）也已接入：保留原功能码与字节，等待匹配的 FB/CB 应答、驱动器目标位置读值（0x33，手册 p70）与两对新鲜静止反馈；同步缓存与 FD 仍只可预览。参数写入需要驱动关闭使能且静止。</p>
    <p className="capabilities__note">{limitsReady ? '运动数值按「调试限制」中已确认的板端策略校验。' : '尚未确认板端限制（/api/limits）：运动指令保持禁用，读取、停止与失能不受影响。'}</p>
    {directNote ? <p className="capabilities__note">直通位置（FB/CB）由板端解析：{directNote}</p> : null}
    <div className="divider"/>
    <details className="device-compat">
      <summary>X 固件 · 默认 0.1°位置输入 · 固定校验 6B</summary>
      <p>协议依据：ZDT_X42S 二代闭环步进电机用户手册 V1.0.5（2026-05-27），页码见各字段提示。</p>
      <p>出厂默认固件是 Emm，报文格式与 X 固件不同；本页不自动识别固件类型、也不换算，接错固件时回包解析不可信。</p>
      <p>本页按默认 0.1°/计数的位置输入解释报文，不支持已配置为 0.01° 输入缩放的驱动器。</p>
      <p>手册描述的能力不等于板端已实现的操作：未知功能码、同步队列与未接入的运动模式仍只作预览，本页不会自行放开。</p>
    </details>
  </section>;
}

export function DeviceApp() {
  useEffect(() => {document.body.classList.add('device-body');return () => document.body.classList.remove('device-body');},[]);
  // Local drafts (browser storage). Read once, before the first render, so the
  // first paint already shows the restored form: no effect can then save a
  // freshly defaulted form over a restored draft.
  const draftsRef = useRef(null);
  if (draftsRef.current === null) draftsRef.current = readDrafts(safeStorage());
  const bootRef = useRef(null);
  if (bootRef.current === null) bootRef.current = bootContext(draftsRef.current);
  const boot = bootRef.current;
  const [draft,setDraft] = useState(boot.motorText), [tab,setTab] = useState('manual');
  const check = validateAddress(draft), address = check.ok ? check.value : 0;
  const [status,setStatus] = useState(EMPTY), [connected,setConnected] = useState(false), [notice,setNotice] = useState('');
  const [busy,setBusy] = useState(false), busyRef = useRef(false);
  const [selected,setSelected] = useState(boot.commandId), [variant,setVariant] = useState(boot.variantKey);
  const [values,setValues] = useState(boot.values);
  const [mode,setMode] = useState(boot.mode), [raw,setRaw] = useState(boot.raw), [dirty,setDirty] = useState(boot.dirty);
  const [query,setQuery] = useState(''), [groups,setGroups] = useState(() => Object.fromEntries(COMMAND_GROUPS.map(g => [g.id,!!g.open])));
  const [manual,setManual] = useState(boot.manual);
  const [pollingBusy, setPollingBusy] = useState(false);
  const [records,setRecords] = useState([]), [filter,setFilter] = useState('all'), [traceQuery,setTraceQuery] = useState(''), [frozen,setFrozen] = useState(null);
  const [labRequest,setLabRequest] = useState(null);
  const labRequestIdRef = useRef(0);
  const traceCursorRef = useRef({generation:0,seq:0});
  const bulkPacketsRef = useRef(new Map());
  // Board motion policy. `limits` stays null until /api/limits answered: the
  // page never assumes 120/240 were loaded, it keeps motion disabled instead.
  const [limits,setLimits] = useState(null), [limitsState,setLimitsState] = useState('loading'), [limitsError,setLimitsError] = useState(null);
  const [limitsSaving,setLimitsSaving] = useState(false), [limitsSaveError,setLimitsSaveError] = useState(null), [limitsSaveNotice,setLimitsSaveNotice] = useState(null);
  const [limitsNonce,setLimitsNonce] = useState(0);
  // Trace coverage problems get their own line near the trace; a request result
  // must never be overwritten by a bus warning.
  const [traceWarning,setTraceWarning] = useState('');
  // Board queue. `queue` is only ever what the board reported; `queueLock`
  // carries the submission states the board has not answered for yet. Only the
  // status poll may clear `unconfirmed` — a lost response stays "unknown"
  // instead of turning into an assumed success or an assumed idle.
  const [queue,setQueue] = useState(null), [queueState,setQueueState] = useState('loading'), [queueError,setQueueError] = useState(null);
  const [queueLock,setQueueLock] = useState({pending:false,unconfirmed:false}), [queueNonce,setQueueNonce] = useState(0);
  const idleStreakRef = useRef(0);
  // 清除板端状态 (POST /api/control/reset). Not a re-enable flow: it takes the
  // board back to a clean volatile state and the operator must enable again
  // explicitly. `resetGeneration` invalidates answers of requests that were
  // already in flight, so a late submission cannot overwrite the reset notice.
  const [resetPending,setResetPending] = useState(false);
  const resetPendingRef = useRef(false);
  const resetGenerationRef = useRef(0);
  // Bumped by every board answer to a start/cancel request. A status read that
  // was already in flight when that answer landed describes the board from
  // before the mutation, so it is discarded instead of overwriting the newer
  // state (and instead of unlocking a queue that is in fact running).
  const queueMutationRef = useRef(0);
  // Poll cadence, shared with the mutation path so a start/cancel answer also
  // speeds the next read up instead of waiting for the slow (idle) interval.
  const queueIntervalRef = useRef(2500);
  const queueRunning = queue?.active === true || queue?.state === 'running';
  const queueStale = queueState === 'error';
  const queueBusy = queueRunning || queueLock.pending || queueLock.unconfirmed;
  const queueUnknown = queueLock.unconfirmed && !queueRunning;
  const targetRef = useRef(address); targetRef.current = address;
  // The lab form the inputs currently belong to, and the motor whose trial-field
  // draft is being edited. Edits are stored under exactly these keys.
  const contextRef = useRef({motorId:boot.motorId, commandId:boot.commandId, variantKey:boot.variantKey});
  const activeMotorRef = useRef(Number(boot.motorId));
  const storageRef = useRef(undefined);
  if (storageRef.current === undefined) storageRef.current = safeStorage();
  const limitsReady = limitsState === 'ready' && limits !== null;
  const experiment = limitsReady ? experimentWindowText(limits) : null;
  const item = cleanItem(getCommandItem(selected));
  const form = encodeCommand({item,variantKey:variant,values,address:address || 1});
  let model = {...form,bytes:form.bytes || [],labels:form.labels || [],errors:form.errors || {},item};
  if (mode === 'raw' && dirty) {
    const parsed = parseLogicalHex(raw);
    const match = parsed.ok && identifyCommandBytes(parsed.bytes,address,selected);
    if (!parsed.ok || !match || parsed.bytes[0] !== address) model = {ok:false,bytes:parsed.bytes || [],labels:[],errors:{_raw:['HEX 格式／指令结构不匹配，或地址与顶部不一致。']}};
    else {
      const result = encodeCommand({item:match.item,variantKey:match.variant.key,values:match.values,address});
      model = {...result,bytes:parsed.bytes,labels:result.labels || [],errors:result.errors || {},item:match.item,identified:true};
    }
  }
  const frames = buildFrames(model.bytes), annotations = frames.map(f => frameAnnotation(model.bytes,model.labels,f));
  const current = connected && status.id === address ? status : {...EMPTY,id:address};
  // Supervised motions the board refuses without a confirmed enable. 0x9A joins
  // them now that homing is gated like every other motion.
  const needsEnable = [0xfb,0xcb,0xcd,0xf5,0xc5,0xf6,0xc6,0x9a].includes(model.bytes[1]);
  const selectedOpcode = Number.isInteger(model.bytes?.[1]) ? model.bytes[1] : null;
  // Reads and stop/interrupt stay usable while a queue runs, and so does the
  // 0xF3 *disable* direction; every other manual mutation (including a manual
  // enable) would interleave with the running program and is refused.
  const queueConflict = queueConflictReason(model.bytes,{running:queueRunning,unknown:queueUnknown});
  const gate = !address ? check.error
    : !connected ? '等待设备连接'
    : !current.canReady ? 'CAN 控制器不可用'
    : busy ? '等待当前请求返回'
    : !model.ok ? '请修正参数'
    // 0x45 (closed-loop maximum phase current) is a parameter write, not a
    // motion, but its value can only be judged against the confirmed limits: it
    // shares this gate without joining the motion list or needing an enable.
    : queueConflict || (isLimitDependentOpcode(selectedOpcode) && !limitsReady ? '未读取到板端限制（/api/limits）：该指令的数值无法校验，已禁用；读取、停止与失能不受影响'
    : supportReason(model.bytes, limitsReady ? limits : null) || (needsEnable && !current.enabled ? '请先发送使能，并等待真实确认' : null));

  useEffect(() => {
    let disposed = false, timer;
    const controller = new AbortController();
    setStatus({...EMPTY,id:address}); setConnected(false);
    async function poll() {
      try {
        if (!address) return;
        const generation = resetGenerationRef.current;
        const next = await request(`/api/status?id=${address}`,undefined,controller.signal);
        if (!disposed && targetRef.current === address && !resetPendingRef.current && generation === resetGenerationRef.current) {setStatus(next);setConnected(true);}
      } catch { if (!disposed) {setConnected(false);setStatus({...EMPTY,id:address});} }
      if (!disposed) timer = setTimeout(poll,300);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[address]);

  // Confirmed limits: refreshed on a timer, on reconnect and when the 调试限制
  // tab is (re)entered, so another browser's save shows up without touching the
  // draft the user is editing (LimitsPanel owns that).
  useEffect(() => {
    let disposed = false, timer = null;
    const controller = new AbortController();
    async function poll() {
      try {
        const payload = await request('/api/limits',undefined,controller.signal);
        if (disposed) return;
        const parsed = readLimitsPayload(payload);
        // Keep the previous object identity when nothing changed so the 2 s
        // poll does not re-render the whole page or the limits draft.
        if (parsed.ok) {setLimits(prev => (prev && limitsEqual(prev,parsed.limits) ? prev : parsed.limits));setLimitsState('ready');setLimitsError(null);}
        else {setLimitsState('error');setLimitsError(parsed.error);}
      } catch (e) {
        if (disposed) return;
        setLimitsState('error');
        setLimitsError(e.status === 404
          ? '板端没有 /api/limits 接口（固件未更新），无法读取限制。'
          : `读取失败：${errorLabels[e.message] || e.message}`);
      }
      if (!disposed) timer = setTimeout(poll,2000);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[limitsNonce,connected,tab]);

  // The queue is polled from the device page, not from the 编排队列 tab, so a
  // program started there keeps gating every other tab. A failed read is not an
  // idle read: the last board-reported status is kept and marked stale, which
  // keeps a running queue locked instead of silently unlocking it.
  useEffect(() => {
    let disposed = false, timer = null;
    const controller = new AbortController();
    async function poll() {
      let next = null, failed = null, failedState = 'error';
      // The mutation counter is read before the request goes out: if a
      // start/cancel answer lands while this read is on the wire, the read is
      // stale by definition and must not be applied when it comes back.
      const epoch = queueMutationRef.current;
      try {
        const payload = await request('/api/queue',undefined,controller.signal);
        if (disposed) return;
        if (!resetPendingRef.current && queueMutationRef.current === epoch) {
          const parsed = readQueueStatus(payload);
          if (parsed.ok) next = parsed.status;
          else failed = parsed.error;
        }
      } catch (e) {
        if (disposed) return;
        if (e.status === 404) {
          failedState = 'unavailable';
          failed = '板端没有 /api/queue 接口（固件未更新）：本页无法读取或提交队列。';
        } else {
          failed = `读取队列状态失败：${errorLabels[e.message] || e.message}`;
        }
      }
      if (next) {
        setQueue(next); setQueueState('ready'); setQueueError(null);
        queueIntervalRef.current = next.state === 'running' ? 800 : 2500;
        if (next.state === 'idle') {
          // One idle answer right after a lost response can still be the state
          // from before the request landed, so an unconfirmed submission is
          // released only after repeated successful idle reads.
          idleStreakRef.current += 1;
          if (idleStreakRef.current >= 3) setQueueLock(lock => (lock.unconfirmed ? {...lock,unconfirmed:false} : lock));
        } else {
          idleStreakRef.current = 0;
          setQueueLock(lock => (lock.unconfirmed ? {...lock,unconfirmed:false} : lock));
        }
      } else if (failed) {
        // The last board-reported status is kept and only marked as stale: a
        // failed read is not evidence that a running queue finished, and it
        // must never unlock one.
        setQueueState(failedState); setQueueError(failed);
      }
      if (!disposed) timer = setTimeout(poll,queueIntervalRef.current);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[queueNonce]);

  useEffect(() => {
    let disposed = false, timer, seq = 0, uptime = 0, generation = 0;
    const controller = new AbortController();
    async function poll() {
      try {
        const data = await request('/api/trace',undefined,controller.signal);
        if (disposed) return;
        if (data.uptimeMs < uptime || data.sequence < seq) {
          seq=0;generation++;
          bulkPacketsRef.current.clear();
          // The board restarted: drop every retained record so decoded replies
          // and diagnostics from the previous run cannot survive the reboot.
          setRecords([]); setFrozen(prev => prev ? [] : null); setTraceWarning('');
          setLabRequest(previous => previous ? {...previous,phase:'restarted',detail:'板端已重启，无法继续确认此前请求'} : previous);
          logStore.noteTraceRestart();
        }
        uptime = data.uptimeMs;
        const incoming = data.frames.filter(f => f.seq > seq);
        const lost = incoming.length && seq && incoming[0].seq > seq+1;
        if (lost) {
          bulkPacketsRef.current.clear();
          setTraceWarning('部分总线记录已被环形缓冲区覆盖；以下仅为保留记录，较早的收发与解析已丢失。');
          logStore.noteTraceGap('总线记录缺口：较早的帧已被板端环形缓冲覆盖，无法补齐');
        }
        const rows = incoming.map(f => {
          // Only the pure manual decoder decides what a reply means; TX frames
          // and anything the manual does not describe stay undecoded.
          let decoded = decodeCanReply({dir:f.dir,id:f.id,extended:f.extended,remote:f.remote,data:f.data});
          if (f.dir === 'RX' && f.extended && !f.remote && f.id >= 0x100 && f.id <= 0xffff && [0x42,0x43].includes(f.data?.[0])) {
            const key = `${f.id >> 8}-${f.data[0]}`;
            const packetIndex = f.id & 0xff;
            if (packetIndex === 0) bulkPacketsRef.current.set(key,[f]);
            else {
              const packets = bulkPacketsRef.current.get(key);
              if (packets?.length === packetIndex && f.atMs >= packets.at(-1).atMs && f.atMs - packets.at(-1).atMs <= 1000) packets.push(f);
              else bulkPacketsRef.current.delete(key);
            }
            const packets = bulkPacketsRef.current.get(key);
            if (packets?.length === 5) {
              decoded = decodeBulkCanReply(packets);
              bulkPacketsRef.current.delete(key);
            }
          }
          const base = `${f.extended ? '扩展帧' : '标准帧'}${f.remote ? ' · 远程帧' : ''} · ${f.dir === 'TX' ? 'TWAI 已入队' : '总线实际接收'}`;
          return {id:`${generation}-${f.seq}`,generation,seq:f.seq,at:Date.now()-(data.uptimeMs-f.atMs),dir:f.dir,canId:f.id,extended:f.extended,remote:f.remote,dlc:f.data.length,data:f.data,decoded:decoded || null,note:decoded ? `${base} · ${decoded.title}：${oneLine(decoded.text)}` : base};
        });
        seq=data.sequence;
        traceCursorRef.current={generation,seq};
        if (rows.length) {
          setRecords(prev => [...prev,...rows].slice(-400));
          // The diagnostic log reuses these already-accepted rows instead of
          // polling /api/trace a second time. Only notable frames are kept.
          logStore.recordTraceRows(rows);
        }
      } catch { /* Status poll owns connection state. Never fabricate RX. */ }
      if (!disposed) timer=setTimeout(poll,500);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[]);

  // Board-side diagnostic log (read-only RAM ring). Polled regardless of the
  // active tab so a fault that happened on another tab is still captured; a
  // missing endpoint slows the poll down instead of pretending the board is
  // offline, and it is reported once by the log store itself.
  const [boardLogNonce,setBoardLogNonce] = useState(0);
  useEffect(() => {
    let disposed = false, timer = null;
    const controller = new AbortController();
    async function poll() {
      let interval = 2000;
      try {
        const payload = await request('/api/logs',undefined,controller.signal);
        if (!disposed) {
          // An endpoint that answers with something that is not this contract is
          // treated like a missing one: one entry, then a slower poll.
          const result = logStore.mergeBoardLog(payload);
          if (result && result.accepted === false) interval = 5000;
        }
      } catch (e) {
        if (!disposed) {
          logStore.noteBoardLogResult({ok:false,status:e.status ?? 0,error:e.message});
          interval = e.status === 404 ? 30000 : 5000;
        }
      }
      if (!disposed) timer = setTimeout(poll,interval);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[boardLogNonce]);

  async function submit(path,data,{stop=false}={}) {
    if (!stop && (busyRef.current || resetPendingRef.current)) return;
    if (!stop) {busyRef.current=true;setBusy(true);}
    const id = data.id ?? address;
    const target = stop && path.endsWith('stop-all') ? '全部' : id;
    // An answer that arrives after a reset describes the state from before it.
    const generation = resetGenerationRef.current;
    const announce = (text) => { if (resetGenerationRef.current === generation) setNotice(text); };
    try {
      const result = await request(path,data);
      if (path === '/api/enable-all') { announce(`全部${Number(data.enabled) ? '使能' : '失能'}广播已发送，未逐台确认`); return {ok:true}; }
      // The trial window is whatever the board is configured with — never a
      // hardcoded number in the page.
      const queued = result?.message === 'queued_experiment' || result?.message === 'queued_auto_stop_5s';
      announce(`电机 ${target}：${queued
        ? `已入队，${experiment || '时长由板端策略决定'}`
        : '请求已入队，等待真实反馈'}`);
      return {ok:true};
    } catch (e) {
      const detail = errorLabels[e.message] || e.message;
      announce(e.uncertain
        ? `${detail}：请求结果未知，超时或断线不代表设备未执行；本页不会自动重发。`
        : `板端拒绝：${detail}（HTTP ${e.status}）`);
      return {ok:false,uncertain:Boolean(e.uncertain),detail};
    }
    // Stop and 全部停止 cancel a running queue on the board (that is the only
    // way the board can guarantee no interleaving), so the queue status is
    // re-read right away instead of waiting for the next poll.
    finally {if (!stop) {busyRef.current=false;setBusy(false);} else setQueueNonce(n=>n+1);}
  }

  async function sendLabCommand() {
    if (gate) return;
    const cursor = traceCursorRef.current;
    const requestId = ++labRequestIdRef.current;
    setLabRequest({requestId,address,opcode:selectedOpcode,generation:cursor.generation,seq:cursor.seq,phase:'sending'});
    const outcome = await submit('/api/command',{hex:formatBytes(model.bytes).replaceAll(' ','')});
    setLabRequest(previous => previous?.requestId === requestId && previous.phase !== 'restarted'
      ? {...previous,phase:outcome?.ok ? 'queued' : outcome?.uncertain ? 'unknown' : 'rejected',detail:outcome?.detail}
      : previous);
  }

  // One immediate status read for paths that changed the board outside the poll
  // (a reset). It never touches `connected`: the poll owns that.
  async function refreshStatus() {
    if (!address) return;
    try {
      const next = await request(`/api/status?id=${address}`);
      if (targetRef.current === address) { setStatus(next); setConnected(true); }
    } catch { /* the status poll owns connection state */ }
  }

  // 清除板端状态: cancels the queue, asks the board to stop once and clears its
  // volatile ownership. It is not a re-enable flow and it proves nothing about
  // the shaft: the page only reports what the board answered and then re-reads
  // the real status, so no confirmation is invented locally.
  async function resetControlState() {
    if (resetPendingRef.current) return;      // one at a time; 全部停止 stays usable
    resetPendingRef.current = true;
    setResetPending(true);
    // Every answer already on the wire describes the state before this reset.
    resetGenerationRef.current += 1;
    queueMutationRef.current += 1;
    try {
      const payload = await request('/api/control/reset',{});
      if (payload?.stateCleared === true) {
        // The board cleared its own bookkeeping. Local ownership is dropped and
        // the state is re-read instead of assumed.
        setQueueLock({pending:false,unconfirmed:false});
        idleStreakRef.current = 0;
        setNotice(payload.stopSent === true
          ? '已清除板端内部状态；停止帧已发送（发送成功不等于电机已物理停止）。需要时请显式重新使能。'
          : `板端已清除内部状态，但停止发送未确认：${errorLabels.control_state_cleared_stop_unconfirmed}`);
      } else {
        setNotice('板端返回了未预期的内容，未清除本地状态；请重新读取状态后再操作。');
      }
      setQueueNonce(n=>n+1);
      refreshStatus();
    } catch (e) {
      if (e.status === 404) {
        setNotice(errorLabels.control_reset_unavailable);
      } else if (e.uncertain) {
        // Unknown means unknown: the local view is not cleared on a guess.
        setNotice(`${errorLabels[e.message] || e.message}：清除结果未知，本页不会自动重试；请重新读取状态判断。`);
      } else if (e.payload?.stateCleared === true) {
        // The board did clear its state; the stop is what is unconfirmed.
        setQueueLock({pending:false,unconfirmed:false});
        setNotice(`板端：${errorLabels[e.message] || errorLabels.control_state_cleared_stop_unconfirmed}（HTTP ${e.status}）`);
        setQueueNonce(n=>n+1);
        refreshStatus();
      } else {
        setNotice(`板端拒绝：${errorLabels[e.message] || e.message}（HTTP ${e.status}）`);
      }
    } finally {
      queueMutationRef.current += 1;
      resetGenerationRef.current += 1;
      resetPendingRef.current = false;
      setResetPending(false);
      setQueueNonce(n=>n+1);
      refreshStatus();
    }
  }

  // The board answers POST /api/limits with the complete saved set; only that
  // response updates the confirmed limits. A failure keeps the old values.
  async function saveLimits(next) {
    setLimitsSaving(true); setLimitsSaveError(null); setLimitsSaveNotice(null);
    // The board refuses configuration writes while a queue owns the bus, so the
    // page never posts one: the refusal is reported instead of a lost request.
    if (queueBusy) {
      setLimitsSaveError(queueRunning
        ? '板端队列正在运行：限制保存会被拒绝，请先「取消队列」或等待结束。'
        : '队列提交结果未知：限制保存会被拒绝，请先「取消队列」核对状态。');
      setLimitsSaving(false);
      return false;
    }
    try {
      const payload = await request('/api/limits',next);
      const parsed = readLimitsPayload(payload);
      if (!parsed.ok) {
        setLimitsSaveError('板端返回的限制不完整或超出实现上限，本地未采用该结果。');
        return false;
      }
      setLimits(parsed.limits); setLimitsState('ready'); setLimitsError(null);
      setLimitsSaveNotice('已保存到板端 NVS，重启后仍然生效；上面显示的是板端返回值。');
      return true;
    } catch (e) {
      const detail = errorLabels[e.message] || e.message;
      setLimitsSaveError(e.uncertain
        ? `${detail}：保存结果未知，本页不会自动重试；实际限制可能未改变。`
        : `保存失败：${detail}`);
      return false;
    } finally {setLimitsSaving(false);}
  }

  // Every edit is written straight away, so a reload never loses it. Context
  // switches only READ: they save the incoming selection, never the outgoing
  // form, which is what keeps a restored draft from being overwritten by the
  // defaults of the form that is being left behind.
  function persist(next) {
    draftsRef.current = next;
    writeDrafts(storageRef.current, next);
  }
  function saveForm(patch, ctx = contextRef.current) {
    persist(putForm(draftsRef.current, ctx.motorId, ctx.commandId, ctx.variantKey, patch));
  }
  function saveManual(next) {
    persist(putManual(draftsRef.current, String(activeMotorRef.current), next));
  }

  // Restores the draft of one motor+command+variant. Defaults are used only for
  // a form that was never edited; a restored draft is never clamped, it is
  // validated against the confirmed limits by the gate like a typed value.
  function applyContext(motorId, commandId, wantedVariant = null) {
    const item = getCommandItem(commandId) ?? getCommandItem('enable');
    const remembered = lastVariantFor(draftsRef.current, motorId, item.id);
    const variantKey = variantKeyOf(item, wantedVariant ?? remembered);
    const stored = getForm(draftsRef.current, motorId, item.id, variantKey);
    const base = deviceDefaults(item, variantKey, limitsReady ? limits : null);
    contextRef.current = {motorId:String(motorId), commandId:item.id, variantKey};
    setSelected(item.id);
    setVariant(variantKey);
    setValues(stored?.values ? {...base, ...pickFields(stored.values, fieldKeys(item, variantKey))} : base);
    setMode(stored?.mode === 'raw' ? 'raw' : 'form');
    setRaw(stored?.raw ?? '');
    setDirty(!!stored?.dirty);
    // Selecting marks this key as the most recently used one, so coming back
    // later returns to this command and variant. Nothing is sent.
    persist(putSelection(
      putForm(draftsRef.current, motorId, item.id, variantKey, {}),
      {motorId:String(motorId), commandId:item.id, variantKey},
    ));
  }

  function choose(id) { applyContext(activeMotorRef.current, id); }
  function changeVariant(key) { applyContext(activeMotorRef.current, selected, key); }

  // The CAN ID field keeps whatever is typed, but only a VALID address switches
  // the draft context: a partial or out-of-range intermediate never overwrites a
  // motor's drafts, and the inactive address still blocks sending as before.
  function changeMotor(text) {
    setDraft(text);
    const next = validateAddress(text);
    if (!next.ok) return;
    if (next.value === activeMotorRef.current) return;
    activeMotorRef.current = next.value;
    setManual({...MANUAL_FIELDS, ...pickFields(getManual(draftsRef.current, next.value) ?? {}, MANUAL_KEYS)});
    applyContext(next.value, selected);
  }

  function changeEditorMode(next) {
    if (next === mode) return;
    // The mode is remembered per key; the HEX text and its "edited" flag are kept
    // as they are, so switching to the form and back never destroys an unsent HEX
    // edit. The explicit reset stays on the existing 「恢复默认」 button.
    setMode(next);
    saveForm({mode:next});
  }

  function editValues(next) { setValues(next); saveForm({values:next}); }
  function editManual(next) { setManual(next); saveManual(next); }
  async function copy(text) {
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text);
      else {const el=document.createElement('textarea');el.value=text;document.body.appendChild(el);el.select();const ok=document.execCommand('copy');el.remove();if (!ok) throw Error();}
      setNotice('已复制');
    } catch {setNotice('复制失败，请手动选择报文。');}
  }
  // The log tab subscribes to the store only while it is visible: the history
  // keeps collecting either way, and the rest of the page is not re-rendered for
  // every observed frame. Restoring history never feeds the controls.
  const [logSnapshot,setLogSnapshot] = useState(() => logStore.snapshot());
  useEffect(() => {
    setLogSnapshot(logStore.snapshot());
    if (tab !== 'log') return undefined;
    return logStore.subscribe(() => setLogSnapshot(logStore.snapshot()));
  },[tab]);
  const logContext = {
    origin: typeof location !== 'undefined' ? location.origin : '',
    boardId: logSnapshot.bootId,
    motorId: address || null,
    // The confirmed set is preferred; what the polls actually observed is the
    // fallback, so an export still describes the board while offline.
    limits: limitsReady ? limits : logSnapshot.limits,
    motor: {...logSnapshot.status, id: address || null},
    queue: logSnapshot.queue,
    config: logSnapshot.config,
  };
  const manualLimits=limitsReady ? limitsToManualLimits(limits) : undefined;
  const prediction=planManualMove(manual,manualLimits);
  const moveEncoded=prediction.ok ? encodeCommand({item:getCommandItem('position'),variantKey:'limit',values:prediction.plan,address:address || 1}) : {bytes:[],labels:[]};
  const moveFrames=buildFrames(moveEncoded.bytes || []);
  const manualGate=!address ? check.error : !connected ? '等待设备连接' : !current.canReady ? 'CAN 控制器不可用' : queueBusy ? (queueRunning ? '板端队列正在运行：常规试动已锁定，请先「取消队列」或使用「全部停止」' : '队列提交结果未知：请先「取消队列」核对状态，再试动') : busy ? '等待请求返回' : !limitsReady ? '未读取到板端限制（/api/limits）：常规试动已禁用' : !prediction.ok ? Object.values(prediction.errors)[0] : !current.enabled ? '请先发送使能，并等待确认' : !current.online ? '等待新鲜电机反馈' : current.fault && current.fault !== 'none' ? '故障未清除，请停止后重新使能' : current.activeId || current.state === 'moving' || current.state === 'homing' || current.state === 'experiment_running' || current.state === 'stop_requested' ? '电机忙，请等待停止' : null;
  const pollingPaused = status.autoQueriesEnabled === false;
  async function togglePolling() {
    if (pollingBusy) return;
    setPollingBusy(true);
    try {
      const result = await request('/api/polling', {enabled: pollingPaused ? 1 : 0});
      if (typeof result.autoQueriesEnabled !== 'boolean') throw new Error('板端未返回轮询状态');
      setStatus(previous => ({...previous, autoQueriesEnabled: result.autoQueriesEnabled}));
    } catch (error) { setNotice(`设置空闲刷新失败：${error.message}，请刷新确认板端状态`); }
    finally { setPollingBusy(false); }
  }
  const shown=(frozen ?? records).filter(r => (filter==='all'||r.dir===filter) && `${formatBytes(r.data)} ${formatCanId(r.canId)} ${r.note}`.toLowerCase().includes(traceQuery.toLowerCase()));
  const labFrames = labRequest ? records.filter(r => r.generation === labRequest.generation && r.seq > labRequest.seq &&
    r.extended && !r.remote && (r.canId >> 8) === labRequest.address && r.data?.[0] === labRequest.opcode) : [];
  const labRx = labFrames.filter(r => r.dir === 'RX');
  const labResponse = labRequest ? {
    ...labRequest,
    txSeen:labFrames.some(r => r.dir === 'TX'),
    rxCount:labRx.length,
    reply:[...labRx].reverse().find(r => r.decoded) || labRx.at(-1) || null,
  } : null;
  // Configuration pages keep their panels; the banner says why a save would be
  // refused instead of letting the board answer with a lost-looking error.
  const queueBanner = queueBusy ? <p className="device-lock-banner" role="status"><GlyphInfo /><span>{queueRunning
    ? '板端队列正在运行：板端会拒绝限制／Wi-Fi 等配置改动，请先「取消队列」或使用「全部停止」。'
    : '队列提交结果未知：配置改动可能被板端拒绝，请先「取消队列」核对状态。'}</span></p> : null;

  const renderGeneration = resetGenerationRef.current;
  return <div className="app device-app">
    <header className="toolbar"><div className="toolbar__brand"><span className="toolbar__logo">Babytech</span><span className="toolbar__divider">/</span><h1 className="toolbar__title">电机协议工作台</h1></div>
      <div className="toolbar__right"><span className="chip chip--soft">实机 · X 协议</span><label className="toolbar__field">CAN ID <input aria-label="CAN ID" className="input input--mono toolbar__address" value={draft} onChange={e=>changeMotor(e.target.value)} inputMode="numeric" aria-invalid={!check.ok}/></label><span>CAN 500 kbit/s</span><span className={`chip ${connected?'chip--ok':'chip--muted'}`}>{connected?'设备在线':'设备未连接'}</span><button type="button" className="button button--outline" disabled={resetPending} title="取消编排队列并尝试停止一次，然后清除板端易失的忙／故障／应答状态；不改配置、不改草稿、不重启。停止帧发送成功也不代表电机已物理停止，之后需要显式重新使能。" onClick={resetControlState}>{resetPending ? '正在清除…' : '清除板端状态'}</button><button type="button" className="button button--outline" disabled={!connected || busy || resetPending || queueBusy} title="广播使能总线上所有电机；不自动开始运动" onClick={()=>submit('/api/enable-all',{enabled:1})}>全部使能</button><button type="button" className="button button--danger" title="取消队列并广播失能，总线上所有电机释放保持力" onClick={()=>submit('/api/enable-all',{enabled:0},{stop:true})}>全部失能</button><button className="button button--danger" onClick={()=>submit('/api/stop-all',{}, {stop:true})}>全部停止</button></div></header>
    <nav className="tabs" role="tablist" aria-label="工作模式">{[['manual','常规试动'],['queue','编排队列'],['demo','屏幕流程'],['scale','称重传感器'],['lab','指令实验室'],['limits','调试限制'],['log','调试日志'],['wifi','Wi-Fi 设置']].map(([id,label])=><button key={id} role="tab" aria-selected={tab===id} className={`tabs__item${tab===id?' is-active':''}`} onClick={()=>setTab(id)}>{label}</button>)}<span className="device-network-note">板端真实接口 · 不自动重发操作</span></nav>
    <ConfigResult/>
    {tab==='queue' ? <QueuePanel connected={connected && !resetPending} limits={limits} limitsReady={limitsReady}
      queue={queue} queueState={queueState} queueError={queueError} queueUnknown={queueUnknown}
      onQueueBusy={next=>{if(renderGeneration===resetGenerationRef.current) setQueueLock(next);}}
      onQueueStatus={next=>{
        if(renderGeneration!==resetGenerationRef.current) return;
        // A start/cancel answer makes any status read that is still on the wire
        // obsolete: bump the counter so the poll drops it instead of letting an
        // older idle/running state overwrite this one.
        queueMutationRef.current += 1;
        queueIntervalRef.current = next.state === 'running' ? 800 : 2500;
        setQueue(next);setQueueState('ready');setQueueError(null);
      }}
      onRefreshQueue={()=>setQueueNonce(n=>n+1)}/>
      : tab==='demo' ? <DemoPanel/> : tab==='scale' ? <ScaleWorkbench/> : tab==='log' ? <DebugLogPanel
        store={logStore} snapshot={logSnapshot} context={logContext}
        onCopy={copy} onClear={()=>{logStore.clear();setLogSnapshot(logStore.snapshot());}}
        onRefresh={()=>setBoardLogNonce(n=>n+1)}/>
      : tab==='wifi' ? <div className="device-tab-fill">{queueBanner}<WifiPanel notify={setNotice}/></div> : tab==='limits' ? <div className="device-tab-fill">{queueBanner}<LimitsPanel
      limits={limits} state={limitsState} loadError={limitsError} connected={connected}
      saving={limitsSaving} saveError={limitsSaveError} saveNotice={limitsSaveNotice}
      onSave={saveLimits} onReload={()=>{setLimitsState('loading');setLimitsError(null);setLimitsNonce(n=>n+1);}} onDraftEdit={()=>{setLimitsSaveError(null);setLimitsSaveNotice(null);}}/></div> : <main className={`workspace workspace--${tab}`}>
      {tab==='lab' ? <><CommandLibrary query={query} onQueryChange={setQuery} openGroups={groups} onToggleGroup={id=>setGroups(g=>({...g,[id]:!g[id]}))} selectedId={selected} onSelect={choose}/>
      <CommandPanel device item={item} variantKey={variant} onVariantChange={changeVariant} values={values} onValueChange={(key,value)=>editValues({...values,[key]:value})} editorMode={mode} onEditorModeChange={changeEditorMode} rawText={dirty?raw:formatBytes(form.bytes || [])} onRawTextChange={text=>{setRaw(text);setDirty(true);saveForm({raw:text,dirty:true});}} onRawTextReplace={text=>{setRaw(text);setDirty(true);saveForm({raw:text,dirty:true});}} rawDirty={dirty} onResetRaw={()=>{setDirty(false);saveForm({dirty:false});}} identifiedItem={model.identified?model.item:null} model={model} frames={frames} annotations={annotations} address={address || 1} addressError={check.ok?null:check.error} gateReason={gate} response={labResponse} onSend={sendLabCommand} onCopy={copy}/></> : <ManualPanel device values={manual} errors={prediction.errors} onChange={(key,value)=>editManual({...manual,[key]:value})} motor={{enabled:current.enabled,positionTenths:current.positionDeg==null?null:current.positionDeg*10}} address={address || 1} prediction={prediction} limits={manualLimits} limitsConfirmed={limitsReady} bytes={moveEncoded.bytes || []} frames={moveFrames} annotations={moveFrames.map(f=>frameAnnotation(moveEncoded.bytes,moveEncoded.labels,f))} gateReason={manualGate} onSend={()=>{if(!manualGate)submit('/api/move',{id:address,angle:Number(manual.angle)*(Number(manual.dir)===1?-1:1),speed:manual.speed,accel:manual.accel,decel:manual.decel,current:manual.current});}} onStop={()=>{if(address)submit('/api/stop',{id:address},{stop:true});}} onCopy={copy} onGoToEnable={()=>{setTab('lab');choose('enable');}}/>}
      <DeviceFeedback status={current} connected={connected} live={connected && status.id === address} notice={notice} lab={tab==='lab'} address={address} opcode={selectedOpcode} records={records} experiment={experiment} limitsReady={limitsReady} queue={queue} queueRunning={queueRunning} queueStale={queueStale} directNote={tab==='lab' ? directPositionBoardNote(model.bytes) : null} draftNote={tab==='lab'||tab==='manual' ? DRAFT_NOTE : null}/>
    </main>}
    {tab!=='scale' && <div className="device-trace">
      {pollingPaused && <p className="device-trace-warning" role="status">空闲页面刷新已暂停；正在执行的指令、await 和同步组仍会主动查询所需反馈。</p>}
      {traceWarning ? <p className="device-trace-warning" role="status"><GlyphInfo /><span>{traceWarning}</span><button type="button" className="link-button" onClick={()=>setTraceWarning('')}>知道了</button></p> : null}
      <TracePanel device pollingPaused={pollingPaused} pollingBusy={pollingBusy} onTogglePolling={togglePolling} records={shown} totalCount={records.length} hiddenCount={0} filter={filter} onFilterChange={setFilter} query={traceQuery} onQueryChange={setTraceQuery} paused={frozen!==null} onTogglePause={()=>setFrozen(frozen?null:[...records])} onClear={()=>{setRecords([]);if(frozen)setFrozen([]);setTraceWarning('');}} onCopy={copy}/>
    </div>}
    {!['manual','lab'].includes(tab) && notice && <div className="toast" role="status">{notice}</div>}
  </div>;
}
