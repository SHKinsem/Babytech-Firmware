import { useMemo, useState } from 'react';
import { GlyphInfo } from './glyphs.jsx';
import { CompactPanel } from './CompactPanel.jsx';
import { LEVELS, MAX_EVENTS, MAX_TRACE_FRAMES, exportJson, exportMarkdown } from '../debug-log.js';

// 调试日志: read-only view of what the page observed.
//
// The panel is a viewer. It never sends anything, never re-requests the board and
// never feeds state back into the controls: a restored history is for reading,
// and every hint here states what the data cannot prove.

const levelLabels = { debug: '调试', info: '信息', warn: '警告', error: '错误' };
const sourceLabels = {
  board: '板端', http: '请求', can: 'CAN', status: '状态', queue: '队列',
  config: '配置', limits: '限制', session: '会话',
};

function download(name, text, type) {
  try {
    const url = URL.createObjectURL(new Blob([text], { type }));
    const link = document.createElement('a');
    link.href = url;
    link.download = name;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
    return true;
  } catch {
    return false;
  }
}

export function DebugLogPanel({ store, snapshot, context, onCopy, onClear, onRefresh }) {
  const [level, setLevel] = useState('all');
  const [source, setSource] = useState('all');
  const [query, setQuery] = useState('');
  const [expanded, setExpanded] = useState(() => new Set());

  const rows = useMemo(() => {
    const needle = query.trim().toLowerCase();
    return [...snapshot.events].reverse().filter((event) => {
      if (level !== 'all' && event.level !== level) return false;
      if (source !== 'all' && event.source !== source) return false;
      if (!needle) return true;
      return `${event.event} ${event.detail} ${event.source} ${event.requestId ?? ''}`.toLowerCase().includes(needle);
    });
  }, [snapshot.events, level, source, query]);

  const boardState = snapshot.boardAvailable === true ? '可读'
    : snapshot.boardAvailable === false ? '不可读（旧固件或读取失败）' : '尚未读取';
  const toggle = (event) => setExpanded((prev) => {
    const next = new Set(prev);
    const key = `${event.at}-${event.requestId ?? ''}-${event.event}`;
    if (next.has(key)) next.delete(key); else next.add(key);
    return next;
  });

  return <main className="workspace workspace--log">
    <CompactPanel title="调试日志" initiallyOpen className="compact-panel--log"><section className="panel panel--log" aria-label="调试日志">
      <div className="log__head">
        <h2 className="panel__title">调试日志</h2>
        <span className="chip chip--soft">本机历史 {snapshot.events.length}/{MAX_EVENTS}</span>
        <span className={`chip ${snapshot.boardAvailable === true ? 'chip--ok' : 'chip--muted'}`}>板端日志：{boardState}</span>
        <span className="chip chip--muted">bootId {snapshot.bootId || '—'}</span>
      </div>
      <p className="capabilities__note">
        只读观察记录：板端日志是 RAM 环形缓冲（最多 48 条），断电重启即丢失，也不是串口控制台全文；本机历史只保存在这个浏览器里。
        只记录实际观察到的 CAN 帧，缺失会标成缺口；从历史恢复的内容仅用于阅读，页面不会据此发送任何指令。
      </p>

      <div className="log__filters">
        <label className="log__field">级别
          <select className="input" aria-label="日志级别筛选" value={level} onChange={(event) => setLevel(event.target.value)}>
            <option value="all">全部</option>
            {LEVELS.map((value) => <option key={value} value={value}>{levelLabels[value]}</option>)}
          </select>
        </label>
        <label className="log__field">来源
          <select className="input" aria-label="日志来源筛选" value={source} onChange={(event) => setSource(event.target.value)}>
            <option value="all">全部</option>
            {Object.keys(sourceLabels).map((value) => <option key={value} value={value}>{sourceLabels[value]}</option>)}
          </select>
        </label>
        <label className="log__field log__field--grow">搜索
          <input className="input" aria-label="日志文本筛选" value={query} onChange={(event) => setQuery(event.target.value)} placeholder="事件 / 详情 / 请求号" />
        </label>
        <button type="button" className="button button--outline" onClick={() => onRefresh && onRefresh()}>重新读取板端日志</button>
      </div>

      <div className="log__actions">
        <button type="button" className="button button--outline" disabled={!rows.length} onClick={() => onCopy(rows.map((event) => `${event.time} ${event.level} ${event.source} ${event.event} ${event.detail}`).join('\n'))}>复制当前筛选</button>
        <button type="button" className="button button--outline" onClick={() => download(`motor-log-${Date.now()}.md`, exportMarkdown(snapshot, context), 'text/markdown')}>导出 Markdown</button>
        <button type="button" className="button button--outline" onClick={() => download(`motor-log-${Date.now()}.json`, exportJson(snapshot, context), 'application/json')}>导出 JSON</button>
        <button type="button" className="button button--outline" onClick={() => onClear && onClear()}>清空本机历史</button>
      </div>
      <p className="capabilities__note">
        导出包含已确认限制、选中电机快照、队列／配置结果（读取到时）、保留的 {snapshot.trace.length}/{MAX_TRACE_FRAMES} 个 CAN 帧、日志版本与来源；
        不含查询串、密码或 Wi-Fi 凭据。导出只读取本机数据，离线也可用。
      </p>

      {snapshot.gaps.length ? <div className="log__gaps" role="status">
        {snapshot.gaps.slice(-3).map((gap) => <p key={`${gap.at}-${gap.from}`}>缺口：seq {gap.from}..{gap.to} · {gap.reason}</p>)}
      </div> : null}

      <ol className="log__rows" aria-label="日志事件">
        {rows.length ? rows.map((event) => {
          const key = `${event.at}-${event.requestId ?? ''}-${event.event}`;
          return <li key={key} className={`log__row log__row--${event.level}`}>
            <span className="log__time">{event.time}</span>
            <span className={`log__level log__level--${event.level}`}>{levelLabels[event.level]}</span>
            <span className="log__source">{sourceLabels[event.source] ?? event.source}</span>
            <div className="log__body">
              <p className="log__event">{event.event}{event.requestId ? <span className="log__request">{event.requestId}</span> : null}</p>
              <p className="log__detail">{event.detail}{event.truncated ? <em>（板端已截断）</em> : null}</p>
              {event.boardMs != null || event.uncertain || event.program ? <button type="button" className="link-button" onClick={() => toggle(event)}>{expanded.has(key) ? '收起' : '详情'}</button> : null}
              {expanded.has(key) ? <div className="log__extra">
                {event.boardMs != null ? <p>板端运行时间 +{(event.boardMs / 1000).toFixed(1)}s（板端时钟，不是本机时间）</p> : null}
                {event.uncertain ? <p>结果未知：请求超时或断线，不代表设备未执行；本页不会自动重发。</p> : null}
                {event.program ? <>
                  <p>提交的队列程序{event.programTruncated ? '（超过 8192 字符，只保留了前 8192 字符）' : ''}：</p>
                  <pre className="log__program">{event.program}</pre>
                </> : null}
              </div> : null}
            </div>
          </li>;
        }) : <li className="log__empty">没有符合筛选条件的事件。</li>}
      </ol>

      <p className="capabilities__note">
        <GlyphInfo /> 「已提交」只表示请求发出，「HTTP 202」只表示板端已接受，「驱动器 ACK」与「动作完成」是另外两件事：
        面板按实际收到的内容分别标注，不按时间先后推断因果。CAN 记录缺失时只标注缺口，不补写。
      </p>

      <h3 className="section__title">保留的 CAN 帧（最近 {snapshot.trace.length} 个，最多 {MAX_TRACE_FRAMES} 个）</h3>
      <ol className="log__frames" aria-label="保留的 CAN 帧">
        {snapshot.trace.length
          ? snapshot.trace.slice(-40).map((frame) => <li key={`${frame.at}-${frame.text}`}><span className="log__time">{frame.time}</span> {frame.text}</li>)
          : <li className="log__empty">尚未观察到控制帧（周期性查询帧不记录）。</li>}
      </ol>
    </section></CompactPanel>
  </main>;
}
