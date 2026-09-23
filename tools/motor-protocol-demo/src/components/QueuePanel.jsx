import { useEffect, useMemo, useRef, useState } from 'react';
import { GlyphInfo, HelpTip } from './glyphs.jsx';
import { QueueDiagnostics } from './QueueDiagnostics.jsx';
import { SyncSettings } from './SyncSettings.jsx';
import { ROTATION_DISTANCE_MAX, ROTATION_DISTANCE_MIN, errorLabels, queueProgressText, queueStateLabels, queueActionLabels, queueMessageText, readMotorDistance, readQueueStatus, request, syncIsolationReasonText } from '../device-api.js';
import {
  QUEUE_LIMITS,
  QUEUE_VERBS,
  SAMPLE_PROGRAM,
  buildActionLine,
  builderDefaults,
  checkRepeat,
  describeAction,
  getVerbDefinition,
  knownEnabledIds,
  validateProgram,
} from '../queue-program.js';

const DRAFT_KEY = 'motor-protocol-demo.queue.draft.v1';
// Verbs that need a confirmed enable before the board will run them.
const MOVE_VERBS = ['move', 'home', 'torque', 'velocity'];

/** Draft persisted between visits. Never starts anything on its own. */
function readDraft() {
  try {
    const raw = localStorage.getItem(DRAFT_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw);
    if (!parsed || typeof parsed !== 'object') return null;
    return {
      program: typeof parsed.program === 'string' ? parsed.program : null,
      repeat: Number.isInteger(parsed.repeat) ? String(parsed.repeat) : null,
    };
  } catch {
    return null;
  }
}

function PreviewRow({ row }) {
  return (
    <li className={`queue-preview__row${row.raw ? ' is-raw' : ''}`}>
      <span className="queue-preview__line" aria-label={`源程序第 ${row.line} 行`}>{row.line}</span>
      <div className="queue-preview__body">
        <p className="queue-preview__summary">{row.summary}</p>
        {row.detail ? <p className="queue-preview__detail">{row.detail}</p> : null}
      </div>
      {row.raw ? <span className="chip chip--warn">无运动监督</span> : null}
    </li>
  );
}

function VerbHelp() {
  return (
    <dl className="queue-help">
      {QUEUE_VERBS.map((entry) => (
        <div className="queue-help__row" key={entry.verb}>
          <dt>
            <code className="queue-help__usage">{entry.usage}</code>
            <span className="queue-help__label">{entry.label}</span>
          </dt>
          <dd>
            <span className="queue-help__form">短写：<code>{entry.shortForm}</code></span>
            <span className="queue-help__form">默认：{entry.defaults}</span>
            <span className="queue-help__note">{entry.note}</span>
          </dd>
        </div>
      ))}
      <p className="queue-help__foot">
        指令与单位不分大小写；数字只接受十进制字面量（可带负号与小数点），不接受 <code>+</code>、<code>0x</code>、科学计数法或任何表达式；
        <code>#</code> 之后是注释。可选参数只能写在最后，越界直接报错、不做截断。
      </p>
    </dl>
  );
}

/**
 * 编排队列: source editor, semantic preview and the board's live progress.
 *
 * The panel owns the queue start/cancel requests and the per-ID rotation
 * distance API; the status poll lives in DeviceApp so a running queue keeps
 * gating the other tabs. Every lock change is reported with its reason through
 * `onQueueBusy({pending, unconfirmed})` — the panel never decides on its own
 * that a run finished.
 */
export function QueuePanel({
  connected,
  limits,
  limitsReady,
  queue,
  queueState,
  queueError,
  queueUnknown,
  onQueueBusy = () => {},
  onQueueStatus = () => {},
  onRefreshQueue = () => {},
}) {
  const [{ program: storedProgram, repeat: storedRepeat }] = useState(() => readDraft() ?? { program: null, repeat: null });
  const [program, setProgram] = useState(storedProgram ?? '');
  const [repeat, setRepeat] = useState(storedRepeat ?? '1');
  const [notice, setNotice] = useState(null);
  const [starting, setStarting] = useState(false);
  const [cancelling, setCancelling] = useState(false);
  const [copied, setCopied] = useState('');
  const startingRef = useRef(false);
  const fileRef = useRef(null);
  const editorRef = useRef(null);

  // --------------------------------------------------------------- rotation distance
  const [distanceId, setDistanceId] = useState('1');
  const [distanceDraft, setDistanceDraft] = useState({ id: '1', value: '', dirty: false });
  const [profiles, setProfiles] = useState({});
  const [distanceSaving, setDistanceSaving] = useState(false);
  const [distanceNotice, setDistanceNotice] = useState(null);
  const loadingRef = useRef(new Set());
  // Reads and writes for the mm/rev profile share one generation counter: a
  // read that started before a save landed describes the board from before it,
  // so it must never be allowed to overwrite the saved value.
  const distanceEpochRef = useRef(0);
  const savingRef = useRef(false);

  // --------------------------------------------------------------- action builder
  const [builderVerb, setBuilderVerb] = useState('move');
  const [builderValues, setBuilderValues] = useState(() => builderDefaults('move'));
  // Off by default: the program is sent exactly as written, with no implicit
  // enable line added for the user.
  const [addEnable, setAddEnable] = useState(false);
  const [builderError, setBuilderError] = useState(null);

  useEffect(() => {
    try {
      localStorage.setItem(DRAFT_KEY, JSON.stringify({ program, repeat }));
    } catch { /* private mode: the draft simply stays in memory */ }
  }, [program, repeat]);

  // Confirmed profiles become the mm/rev table the preview may use. A failed
  // read stays absent: the preview must not turn it into an assumed default.
  const distances = useMemo(() => {
    const out = {};
    Object.entries(profiles).forEach(([id, profile]) => {
      if (profile.state === 'ready') out[id] = profile.value;
      else if (profile.state === 'none') out[id] = null;
    });
    return out;
  }, [profiles]);

  const result = useMemo(() => validateProgram(program, { distances }), [program, distances]);
  const repeatCheck = checkRepeat(repeat);
  const mmIds = useMemo(
    () => [...new Set(result.actions.flatMap(a=>a.verb==='helix'?[a.linearId]:a.verb==='move'&&a.unit==='mm'?[a.id]:[]))],
    [result.actions],
  );
  const mmIdKey = mmIds.join(',');
  // Enable state at the end of the program, not every enable line ever written.
  const enabledIds = useMemo(() => knownEnabledIds(result.actions), [result.actions]);

  async function loadProfile(id, { force = false } = {}) {
    if (!connected || !Number.isInteger(id) || id < 1) return;
    if (loadingRef.current.has(id)) return;
    const known = profiles[id]?.state;
    if (!force && (known === 'ready' || known === 'none')) return;
    loadingRef.current.add(id);
    const epoch = distanceEpochRef.current;
    const before = profiles[id];
    setProfiles((prev) => ({ ...prev, [id]: { state: 'loading' } }));
    try {
      const payload = await request(`/api/motor-distance?id=${id}`);
      // A save answered while this read was in flight: drop the stale answer
      // instead of letting it overwrite the value the board just confirmed.
      // Only the read's own "loading" marker is taken back.
      if (distanceEpochRef.current !== epoch) {
        setProfiles((prev) => (prev[id]?.state === 'loading' ? { ...prev, [id]: before ?? { state: 'unknown' } } : prev));
        return;
      }
      const parsed = readMotorDistance(payload, id);
      if (!parsed.ok) {
        setProfiles((prev) => ({ ...prev, [id]: { state: 'error', error: parsed.error } }));
        return;
      }
      setProfiles((prev) => ({
        ...prev,
        [id]: parsed.value === null ? { state: 'none', value: null } : { state: 'ready', value: parsed.value },
      }));
    } catch (error) {
      if (distanceEpochRef.current !== epoch) return;
      setProfiles((prev) => ({
        ...prev,
        [id]: {
          state: 'error',
          error: error.status === 404
            ? '板端没有 /api/motor-distance 接口（固件未更新）。'
            : `读取失败：${errorLabels[error.message] || error.message}`,
        },
      }));
    } finally {
      loadingRef.current.delete(id);
    }
  }

  // mm actions need the confirmed per-ID distance; the editor's ID is fetched
  // when it is selected. Both only ever read.
  useEffect(() => {
    mmIdKey.split(',').filter(Boolean).forEach((id) => loadProfile(Number(id)));
  }, [mmIdKey, connected]);

  useEffect(() => {
    loadProfile(Number(distanceId));
  }, [distanceId, connected]);

  // A poll refresh must never overwrite the value the user is typing. The
  // editor only follows the confirmed profile while the field is untouched.
  useEffect(() => {
    if (distanceDraft.dirty) return;
    const profile = profiles[distanceId];
    const next = profile?.state === 'ready' ? String(profile.value) : '';
    setDistanceDraft((prev) => (
      prev.id === distanceId && prev.value === next && !prev.dirty
        ? prev
        : { id: distanceId, value: next, dirty: false }
    ));
  }, [distanceId, profiles, distanceDraft.dirty]);

  // Configuration writes are refused by the board while a queue owns the bus,
  // so the page reports the lock instead of posting a request that will fail.
  const configLocked = queue?.active || queue?.state === 'running' || queueUnknown;

  async function saveDistance(clear) {
    // Synchronous guard: a double click (or a click racing the disabled state)
    // must not put two writes for the same profile on the bus.
    if (savingRef.current) return;
    const id = Number(distanceId);
    const requested = clear ? 0 : Number(distanceDraft.value);
    setDistanceNotice(null);
    if (configLocked) {
      setDistanceNotice({
        tone: 'error',
        text: queue?.state === 'running'
          ? '板端队列正在运行：旋转距离保存会被拒绝，请先「取消队列」或等待结束。'
          : '队列提交结果未知：保存会被拒绝，请先「取消队列」核对状态。',
      });
      return;
    }
    if (!/^\d+$/.test(distanceId.trim()) || id < QUEUE_LIMITS.addressMin || id > QUEUE_LIMITS.addressMax) {
      setDistanceNotice({ tone: 'error', text: `电机地址需在 ${QUEUE_LIMITS.addressMin}..${QUEUE_LIMITS.addressMax} 之间` });
      return;
    }
    if (!clear && !(Number.isFinite(requested) && requested >= ROTATION_DISTANCE_MIN && requested <= ROTATION_DISTANCE_MAX)) {
      setDistanceNotice({ tone: 'error', text: `旋转距离需在 ${ROTATION_DISTANCE_MIN}..${ROTATION_DISTANCE_MAX} mm/rev 之间；要清除请用「清除（写 0）」。` });
      return;
    }
    savingRef.current = true;
    const epoch = ++distanceEpochRef.current;
    setDistanceSaving(true);
    try {
      const payload = await request('/api/motor-distance', { id, rotationDistance: requested });
      if (epoch !== distanceEpochRef.current) return;
      const parsed = readMotorDistance(payload, id);
      if (!parsed.ok) {
        // A mismatched or out-of-range answer is never adopted onto whichever
        // ID the form happens to show now; the board is re-read instead.
        setDistanceNotice({ tone: 'error', text: parsed.mismatch ? parsed.error : `板端返回的旋转距离不可信：${parsed.error}` });
        loadProfile(id, { force: true });
        return;
      }
      setProfiles((prev) => ({ ...prev, [id]: parsed.value === null ? { state: 'none', value: null } : { state: 'ready', value: parsed.value } }));
      // The draft only follows the confirmed value while it still belongs to
      // this address: a value saved for one motor is never shown for another.
      setDistanceDraft((prev) => (prev.id === String(id)
        ? { id: String(id), value: parsed.value === null ? '' : String(parsed.value), dirty: false }
        : prev));
      setDistanceNotice({
        tone: 'ok',
        text: parsed.value === null
          ? `已清除电机 ${id} 的旋转距离：板端返回 0，使用 mm 的程序会被拒绝。`
          : `已保存电机 ${id} 的旋转距离：${parsed.value} mm/rev（板端返回值）。`,
      });
    } catch (error) {
      const detail = errorLabels[error.message] || error.message;
      setDistanceNotice({
        tone: 'error',
        text: error.uncertain
          ? `${detail}：保存结果未知，本页不会自动重试，板端数值可能未改变。`
          : `保存失败（HTTP ${error.status}）：${detail}`,
      });
    } finally {
      if (epoch === distanceEpochRef.current) {
        savingRef.current = false;
        setDistanceSaving(false);
      }
    }
  }

  // --------------------------------------------------------------- start / cancel
  const startBlock = !connected
    ? '设备未连接：无法提交队列。'
    : queueState === 'unavailable'
      ? '板端没有 /api/queue 接口（固件未更新），本页无法提交队列。'
      : queue?.active || queue?.state === 'running'
        ? '队列正在运行：请先「取消队列」或等待板端结束。'
        : queueUnknown
          ? '上一次提交的结果未知：请先「取消队列」确认板端状态。'
          : !result.ok
            ? '请先修正程序中的错误。'
            : !repeatCheck.ok
              ? repeatCheck.error
              : null;
  const canStart = startBlock === null && !starting && !cancelling;

  async function start() {
    if (startingRef.current || startBlock) return;
    startingRef.current = true;
    setStarting(true);
    setNotice(null);
    // Lock synchronously: a second click (or a second tab action) must not be
    // able to submit the same program twice.
    onQueueBusy({ pending: true, unconfirmed: false });
    try {
      const payload = await request('/api/queue/start', { program, repeat: repeatCheck.value });
      const parsed = readQueueStatus(payload);
      if (parsed.ok) {
        // The 202 body is the board's own status, not an optimistic guess.
        onQueueStatus(parsed.status);
        onQueueBusy({ pending: false, unconfirmed: false });
        setNotice({
          tone: parsed.status.state === 'failed' ? 'error' : 'ok',
          text: `板端已接受队列（HTTP 202）：${queueProgressText(parsed.status)}。默认发送后继续；move/home 末尾加 await 才等待完成；原始帧（hex / can）只报告已发送，不推断完成。`,
        });
      } else {
        // 202 without a readable body still means the board took the program.
        onQueueBusy({ pending: false, unconfirmed: true });
        setNotice({ tone: 'warn', text: '板端已接受提交（HTTP 202），但返回的状态无法解析：执行状态以板端轮询为准。' });
      }
    } catch (error) {
      const detail = errorLabels[error.message] || error.message;
      if (error.uncertain) {
        onQueueBusy({ pending: false, unconfirmed: true });
        setNotice({
          tone: 'warn',
          text: `${detail}：提交结果未知，板端可能已经开始执行；本页不会自动重发。请点「刷新状态」核对，必要时直接「取消队列」。`,
        });
      } else {
        const line = error.payload?.line ?? error.payload?.programLine;
        const cause = error.message === 'sync_cache_isolation_unverified' && error.payload?.isolationReason
          ? ` 板端记录原因：${syncIsolationReasonText(error.payload.isolationReason)}。` : '';
        // A refusal is a known outcome: the board answered, so nothing is
        // running because of this request. 400 means the program never started;
        // 409 means something else owns the bus and the status poll will show it.
        onQueueBusy({ pending: false, unconfirmed: false });
        setNotice({
          tone: 'error',
          text: `板端拒绝（HTTP ${error.status}）${Number.isInteger(Number(line)) && Number(line) > 0 ? ` · 源程序第 ${line} 行` : ''}：${detail}${cause}`,
        });
        onRefreshQueue();
      }
    } finally {
      startingRef.current = false;
      setStarting(false);
    }
  }

  async function cancel() {
    if (cancelling) return;
    setCancelling(true);
    setNotice(null);
    try {
      const payload = await request('/api/queue/cancel', {});
      const parsed = readQueueStatus(payload);
      if (parsed.ok) {
        onQueueStatus(parsed.status);
        onQueueBusy({ pending: false, unconfirmed: false });
        setNotice({ tone: 'ok', text: `取消已由板端确认：${queueProgressText(parsed.status)}。剩余动作不会执行。` });
      } else {
        onQueueBusy({ pending: false, unconfirmed: true });
        setNotice({ tone: 'warn', text: '取消请求已被接受，但返回状态无法解析：请用「刷新状态」核对。' });
      }
    } catch (error) {
      const detail = errorLabels[error.message] || error.message;
      if (!error.uncertain && error.status === 404) {
        setNotice({ tone: 'error', text: '板端没有取消接口（固件未更新）：请使用「全部停止」。' });
      } else {
        onQueueBusy({ pending: false, unconfirmed: true });
        setNotice({
          tone: 'warn',
          text: `${detail}：取消结果未知，板端可能仍在执行；本页不会自动重发。可再次点击「取消队列」或使用「全部停止」。`,
        });
      }
    } finally {
      setCancelling(false);
    }
  }

  // --------------------------------------------------------------- editor helpers
  async function copyProgram() {
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(program);
      else {
        const area = document.createElement('textarea');
        area.value = program;
        document.body.appendChild(area);
        area.select();
        const ok = document.execCommand('copy');
        area.remove();
        if (!ok) throw new Error('copy_failed');
      }
      setCopied('已复制程序文本');
    } catch {
      setCopied('复制失败，请手动选择文本。');
    }
  }

  function exportProgram() {
    try {
      const blob = new Blob([program], { type: 'text/plain;charset=utf-8' });
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = 'queue-program.txt';
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
      setCopied('已导出 queue-program.txt');
    } catch {
      setCopied('导出失败，请改用「复制」。');
    }
  }

  async function importProgram(event) {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;
    try {
      setProgram(await file.text());
      setCopied(`已导入 ${file.name}（仅本地草稿，尚未提交板端）`);
    } catch {
      setCopied('导入失败，请改用「复制／粘贴」。');
    }
  }

  function insertAction() {
    const built = buildActionLine(builderVerb, builderValues);
    if (!built.ok) {
      setBuilderError(built.error);
      return;
    }
    setBuilderError(null);
    const lines = [];
    if (addEnable && MOVE_VERBS.includes(builderVerb) && !enabledIds.has(built.action.id)) lines.push(`enable ${built.action.id}`);
    lines.push(built.line);
    setProgram((prev) => {
      const base = prev.replace(/\s+$/, '');
      return `${base ? `${base}\n` : ''}${lines.join('\n')}\n`;
    });
  }

  const definition = getVerbDefinition(builderVerb);
  const builderPreview = (() => {
    const built = buildActionLine(builderVerb, builderValues);
    return built.ok ? describeAction(built.action, { distances }) : built.error;
  })();

  // A failed read keeps the last board-reported status on screen but never
  // presents it as current: the label and the note say which one it is.
  const queueStale = queueState === 'error';
  const lastStateLabel = queue ? (queue.state==='done' ? (queue.motionComplete?'运动完成':'发送结束') : queueStateLabels[queue.state]) : null;
  const stateLabel = queueState === 'unavailable'
    ? '不可用'
    : queueStale && lastStateLabel ? `${lastStateLabel}（最后读取，当前未知）` : (lastStateLabel ?? queueStateLabels.unknown);
  const chipClass = queue?.state === 'running' ? 'chip--warn' : queue?.state === 'failed' ? 'chip--warn' : queue?.state === 'done' && !queue?.raw ? 'chip--ok' : 'chip--muted';

  return (
    <div className="queue-page">
      <QueueDiagnostics queue={queue} editorRef={editorRef} notice={notice} />
      <section className="queue-col queue-col--source" aria-label="队列程序">
        <div className="queue-col__head">
          <h2 className="panel__title">编排队列</h2>
          <span className={`chip ${chipClass}`}>板端队列：{stateLabel}</span>
        </div>
        <details><summary>执行规则与注意事项</summary><p className="panel__desc">
          按顺序发送，默认发送后继续；move/home 末尾加 await 才等待本次动作完成。速度／力矩按写出的持续时间执行；<code>wait MS</code> 用于额外延时。await 期间反馈中断时停留当前行并提示，恢复后继续；驱动拒绝时报告原因，不自动失能或追加停机。原始帧只负责发送。浏览器断开后板端仍继续，不会自动重发。
        </p></details>

        <label className="queue-editor__label" htmlFor="queue-program">
          队列程序（每行一个动作，<code>#</code> 注释）
        </label>
        <textarea
          ref={editorRef}
          id="queue-program"
          className="queue-editor"
          aria-label="队列程序"
          spellCheck={false}
          autoComplete="off"
          value={program}
          onChange={(event) => { setProgram(event.target.value); setNotice(null); }}
          placeholder={'enable 1\nmove 1 90 await\nhome 2 0 await\ntorque 3 -300 1500'}
        />
        <p className="queue-stats">
          动作 {result.stats.actions}/{QUEUE_LIMITS.maxActions} · 文本 {result.stats.bytes}/{QUEUE_LIMITS.maxTextBytes} 字节 ·
          使用地址 {result.stats.usedIds.length ? result.stats.usedIds.join('、') : '—'}
        </p>

        <div className="queue-buttons">
          <button type="button" className="link-button" onClick={() => { setProgram(SAMPLE_PROGRAM); setNotice(null); }}>载入示例</button>
          <button type="button" className="link-button" onClick={copyProgram}>复制</button>
          <button type="button" className="link-button" onClick={exportProgram}>导出</button>
          <button type="button" className="link-button" onClick={() => fileRef.current?.click()}>导入</button>
          <button type="button" className="link-button" onClick={() => { setProgram(''); setNotice(null); }}>清空</button>
          <input ref={fileRef} type="file" accept=".txt,.queue,text/plain" className="queue-file" aria-label="导入队列程序文件" onChange={importProgram} />
        </div>
        {copied ? <p className="queue-stats" role="status">{copied}</p> : null}

        {result.errors.length ? (
          <ul className="queue-errors" aria-label="程序错误">
            {result.errors.map((error, index) => (
              <li key={`${error.line}-${index}`}>
                <strong>{error.line > 0 ? `第 ${error.line} 行` : '程序'}</strong>：{error.message}
              </li>
            ))}
          </ul>
        ) : null}

        <div className="queue-submit">
          <label className="queue-submit__repeat" htmlFor="queue-repeat">
            重复次数
            <input
              id="queue-repeat"
              className="input input--mono"
              inputMode="numeric"
              aria-label="重复次数"
              aria-invalid={repeatCheck.ok ? undefined : 'true'}
              value={repeat}
              onChange={(event) => { setRepeat(event.target.value); setNotice(null); }}
            />
            <span className="field__range">1..1000</span>
          </label>
          <button type="button" className="button button--primary" onClick={start} disabled={!canStart}>
            {starting ? '提交中…' : '开始执行'}
          </button>
          <button type="button" className="button button--danger" onClick={cancel} disabled={cancelling}>
            {cancelling ? '取消中…' : '取消队列'}
          </button>
        </div>

        {startBlock ? (
          <p className="queue-gate" role="status"><GlyphInfo /><span>{startBlock}</span></p>
        ) : (
          <p className="queue-gate"><GlyphInfo /><span>提交后本页不会自动重试；运行状态只以板端返回为准。</span></p>
        )}
        {notice && notice.tone!=='error' ? (
          <p className={`queue-notice queue-notice--${notice.tone}`} role={notice.tone === 'error' ? 'alert' : 'status'}>{notice.text}</p>
        ) : null}

        <section className="queue-builder" aria-label="动作插入">
          <h3 className="section__title">动作插入（可选）</h3>
          <div className="field">
            <span className="field__label"><span>指令</span></span>
            <div className="field__control">
              <select
                className="input"
                aria-label="插入指令"
                value={builderVerb}
                onChange={(event) => {
                  const verb = event.target.value;
                  setBuilderVerb(verb);
                  setBuilderValues(builderDefaults(verb));
                  setBuilderError(null);
                }}
              >
                {QUEUE_VERBS.map((entry) => <option key={entry.verb} value={entry.verb}>{entry.verb} · {entry.label}</option>)}
              </select>
            </div>
          </div>
          <div className="queue-builder__args">
            {definition.args.map((arg) => (
              <label key={arg.key} className="queue-builder__arg">
                <span>{arg.label}{arg.unit ? `（${arg.unit}）` : ''}</span>
                <input
                  className="input input--mono"
                  aria-label={`${definition.verb} ${arg.label}`}
                  value={builderValues[arg.key] ?? ''}
                  onChange={(event) => {
                    setBuilderValues((prev) => ({ ...prev, [arg.key]: event.target.value }));
                    setBuilderError(null);
                  }}
                />
              </label>
            ))}
          </div>
          {MOVE_VERBS.includes(builderVerb) ? (
            <label className="queue-builder__check">
              <input
                type="checkbox"
                checked={addEnable}
                onChange={(event) => setAddEnable(event.target.checked)}
              />
              <span>若程序里还没有该地址的 enable，就在前面补一行</span>
            </label>
          ) : null}
          <p className="queue-builder__preview">{builderPreview}</p>
          <button type="button" className="button button--outline" onClick={insertAction}>插入到程序末尾</button>
          {builderError ? <p className="field__error" role="alert">{builderError}</p> : null}
        </section>
      </section>

      <section className="queue-col queue-col--preview" aria-label="语义预览">
        <div className="queue-col__head">
          <h2 className="panel__title">语义预览</h2>
          <span className="chip chip--soft">{result.preview.length} 个动作</span>
        </div>
        <p className="panel__desc">
          按源行预览动作；板端执行最终换算。普通队列只检查协议范围，mm 使用该地址保存的 mm/rev。
        </p>
        {result.preview.length ? (
          <ol className="queue-preview">
            {result.preview.map((row) => <PreviewRow key={`${row.line}-${row.verb}`} row={row} />)}
          </ol>
        ) : (
          <p className="queue-empty">
            程序为空：点「载入示例」查看三台电机的示例（enable → move → home → torque → stop），或直接在左侧写一行动作。
          </p>
        )}
        {result.warnings.length ? (
          <ul className="queue-warnings" aria-label="预览提醒">
            {result.warnings.map((warning, index) => <li key={`${warning.line}-${index}`}>第 {warning.line} 行：{warning.message}</li>)}
          </ul>
        ) : null}
        <p className="queue-col__foot">
          原始帧（hex / can）只报告“已发送”，不代表电机已动作或已停止；原始步骤之后需要停止时，请显式写 stop 或 disable。
        </p>
      </section>

      <section className="queue-col queue-col--side" aria-label="板端进度与旋转距离">
        <div className="queue-col__head">
          <h2 className="panel__title">板端进度</h2>
          <button type="button" className="link-button" onClick={onRefreshQueue}>刷新状态</button>
        </div>
        <dl className="queue-progress">
          <div><dt>状态</dt><dd>{stateLabel}</dd></div>
          <div><dt>步骤</dt><dd>{queue && queue.total ? `${queue.step}/${queue.total}` : (queue?.step || '—')}</dd></div>
          <div><dt>轮次</dt><dd>{queue && queue.repeat ? `${queue.iteration || 1}/${queue.repeat}` : '—'}</dd></div>
          <div><dt>源程序行</dt><dd>{queue?.line ? `第 ${queue.line} 行` : '—'}</dd></div>
          <div><dt>当前动作</dt><dd>{queueActionLabels[queue?.action] || queue?.action || '—'}</dd></div>
          <div><dt>含原始帧</dt><dd>{queue ? (queue.raw ? '是（无运动监督）' : '否') : '—'}</dd></div>
        </dl>
        <p className="queue-progress__message">{queueMessageText(queue?.message) || '板端尚未报告消息。'}</p>
        {queue?.sync?.phase && queue.sync.phase!=='idle'?<section aria-label="同步成员状态">
          <p>同步阶段：{queue.sync.phase} · 可观测偏差下界 {queue.sync.errorLower ?? '—'} / 上界 {queue.sync.errorUpper ?? '—'}</p>
          {queue.sync.helixErrorLowerMm!=null?<p>轴向偏差区间 {queue.sync.helixErrorLowerMm}–{queue.sync.helixErrorUpperMm} mm（非连续精度保证）</p>:null}
          <ul>{(queue.sync.members??[]).map(m=><li key={m.id}>电机 {m.id} · 行 {m.line}：{m.stopped?'已观察到静止':m.stopSent?'停止已发送，静止未确认':queue.sync.error?'停止未发送成功，静止未知':m.done?'到位已确认':m.targetConfirmed?'目标读回匹配':m.accepted?'收到接受应答（无事务序号）':'准备中'}</li>)}</ul>
        </section>:null}
        {queueUnknown ? (
          <p className="queue-notice queue-notice--warn" role="status">
            提交或取消的结果未知：板端可能仍在执行。本页持续读取 /api/queue，状态只会以板端返回为准。
          </p>
        ) : null}
        {queueState === 'unavailable' || queueState === 'error' ? (
          <p className="queue-notice queue-notice--warn" role="status">
            {queueError}
            {queue && queueStale ? ' 上面显示的是最后一次成功读取的板端状态；读取失败不会解除锁定，也不会把运行中的队列当作已结束。' : ''}
          </p>
        ) : null}

        <div className="divider" />
        <SyncSettings connected={connected} locked={configLocked} />
        <h3 className="section__title">旋转距离（mm/rev）</h3>
        <p className="queue-col__foot">
          mm 单位依赖每个地址自己的旋转距离，保存在板端 NVS。未读取到就是未读取到：本页不会假定默认值；写 0 表示显式清除。
        </p>
        <div className="queue-distance">
          <label>
            <span>电机地址</span>
            <input
              className="input input--mono"
              inputMode="numeric"
              aria-label="旋转距离电机地址"
              disabled={distanceSaving}
              value={distanceId}
              onChange={(event) => {
                // Switching IDs drops the previous draft: a value typed for one
                // motor must never be saved onto another.
                setDistanceId(event.target.value);
                setDistanceDraft({ id: event.target.value, value: '', dirty: false });
                setDistanceNotice(null);
              }}
            />
          </label>
          <label>
            <span>旋转距离（mm/rev）</span>
            {/* Both fields freeze while a save is in flight: an answer can only
                be applied to the address it was requested for. */}
            <input
              className="input input--mono"
              inputMode="decimal"
              aria-label="旋转距离 mm/rev"
              disabled={distanceSaving}
              placeholder={profiles[distanceId]?.state === 'none' ? '未配置（0）' : '未读取'}
              value={distanceDraft.value}
              onChange={(event) => setDistanceDraft({ id: distanceId, value: event.target.value, dirty: true })}
            />
          </label>
          <div className="queue-distance__actions">
            <button type="button" className="button button--outline" onClick={() => saveDistance(false)} disabled={distanceSaving || !connected || configLocked}>保存到板上</button>
            <button type="button" className="button button--outline" onClick={() => saveDistance(true)} disabled={distanceSaving || !connected || configLocked}>清除（写 0）</button>
          </div>
        </div>
        <p className="queue-stats">
          电机 {distanceId}：
          {(() => {
            const profile = profiles[distanceId];
            if (profile?.state === 'ready') return `已确认 ${profile.value} mm/rev`;
            if (profile?.state === 'none') return '已确认为 0（未配置）';
            if (profile?.state === 'loading') return '读取中…';
            if (profile?.state === 'error') return profile.error;
            return '未读取';
          })()}
        </p>
        {distanceNotice ? (
          <p className={`queue-notice queue-notice--${distanceNotice.tone}`} role={distanceNotice.tone === 'error' ? 'alert' : 'status'}>{distanceNotice.text}</p>
        ) : null}
        {mmIds.length ? (
          <p className="queue-col__foot">
            mm 行程使用：{mmIds.map((id) => {
              const profile = profiles[id];
              const text = profile?.state === 'ready' ? `${id} = ${profile.value} mm/rev`
                : profile?.state === 'none' ? `${id} 未配置`
                  : `${id} 未读取`;
              return text;
            }).join(' · ')}
          </p>
        ) : null}

        <div className="divider" />
        <details><summary>指令与默认值</summary><h3 className="section__title">
          指令与默认值
          <HelpTip label="编排队列" text="板端先校验整份程序，再按顺序直接发送；只检查报文编码、单位换算和资源容量所需边界。速度、电流和行程由操作者按实机条件判断。" />
        </h3>
        <VerbHelp />
        </details>
      </section>
    </div>
  );
}
