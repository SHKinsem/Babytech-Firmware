import { useEffect, useMemo, useRef, useState } from 'react';

import { request } from '../device-api.js';
import { CompactPanel } from './CompactPanel.jsx';
import { GlyphDot, GlyphInfo, GlyphSearch } from './glyphs.jsx';

const EMPTY_SCALE = {
  initialized: false,
  doutPin: null,
  sckPin: null,
  gpio45Allowed: false,
  available: false,
  status: 'disabled',
  calibrated: false,
  stable: false,
  rawCounts: null,
  netCounts: null,
  weightG: null,
  sampleAgeMs: null,
  sampleCount: 0,
  tareInProgress: false,
  tareCompleted: false,
  calibrationPersisted: false,
  tareRaw: 0,
  countsPerGram: null,
};

const STATUS_LABELS = {
  disabled: '传感器未启用',
  warming_up: '正在预热',
  uncalibrated: '等待校准',
  unstable: '读数波动',
  stable: '读数稳定',
  stale: '数据已超时',
  fault: '传感器故障',
};

const SCALE_ERRORS = {
  motion_active: '电机正在运动，请停止后再操作',
  scale_unavailable: '称重传感器不可用',
  tare_already_active: '去皮已经在进行',
  tare_required: '请先完成去皮',
  fresh_scale_sample_required: '需要一条新鲜的称重采样',
  calibration_failed: '校准失败，请检查砝码和读数',
  scale_pin_invalid_or_reserved: 'GPIO 不可用、已被占用，或 DOUT 与 SCK 相同',
  scale_pin_save_failed: 'IO 配置保存失败',
  scale_reconfigure_failed: 'HX711 使用新 IO 初始化失败',
};

const BASE_SCALE_PINS = new Set([1,2,6,7,8,9,10,11,12,13,14,15,16,17,18,21,38,39,40,41,42,47,48]);

const pad = (value, width = 2) => String(value).padStart(width, '0');
const formatTime = timestamp => {
  const date = new Date(timestamp);
  return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}.${pad(date.getMilliseconds(), 3)}`;
};
const formatNumber = (value, digits = 2) => Number.isFinite(value) ? Number(value).toFixed(digits) : '—';
const formatInteger = value => Number.isFinite(value) ? Math.round(value).toLocaleString('zh-CN') : '—';

function deriveDiagnostics(history, scale, connected) {
  const points = history.filter(point => Number.isFinite(point.weight));
  const first = points[0], last = points.at(-1);
  const drift = first && last ? last.weight - first.weight : null;
  const recent = points.filter(point => !last || point.at >= last.at - 10000);
  const values = recent.map(point => point.weight);
  const noise = values.length > 1 ? Math.max(...values) - Math.min(...values) : null;
  const countDelta = first && last ? last.sampleCount - first.sampleCount : 0;
  const elapsedSeconds = first && last ? (last.at - first.at) / 1000 : 0;
  const sampleRate = elapsedSeconds > 0 && countDelta >= 0 ? countDelta / elapsedSeconds : null;
  let stableSince = null;
  for (let index = history.length - 1; index >= 0; index -= 1) {
    if (!history[index].stable) break;
    stableSince = history[index].at;
  }
  const stableSeconds = stableSince && last ? (last.at - stableSince) / 1000 : 0;
  let label = '无数据', tone = 'muted';
  if (!connected || !scale.initialized) label = '设备离线';
  else if (scale.status === 'fault') { label = '传感器故障'; tone = 'danger'; }
  else if (scale.status === 'stale') { label = '采样超时'; tone = 'danger'; }
  else if (!scale.calibrated) { label = '等待校准'; tone = 'warn'; }
  else if (!scale.stable) { label = '读数波动'; tone = 'warn'; }
  else if (Number.isFinite(drift) && Math.abs(drift) > 0.2) { label = '轻微漂移'; tone = 'warn'; }
  else { label = '稳定'; tone = 'ok'; }
  return { points, drift, noise, sampleRate, stableSeconds, label, tone };
}

function ScaleChart({ points }) {
  const canvasRef = useRef(null);

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return undefined;
    const draw = () => {
      const rect = canvas.getBoundingClientRect();
      const ratio = window.devicePixelRatio || 1;
      canvas.width = Math.max(1, Math.round(rect.width * ratio));
      canvas.height = Math.max(1, Math.round(rect.height * ratio));
      const context = canvas.getContext('2d');
      context.setTransform(ratio, 0, 0, ratio, 0, 0);
      context.clearRect(0, 0, rect.width, rect.height);

      const plot = { left: 54, top: 14, right: rect.width - 16, bottom: rect.height - 34 };
      const width = Math.max(1, plot.right - plot.left);
      const height = Math.max(1, plot.bottom - plot.top);
      const series = points.length ? points.map(point => ({ ...point, drift: point.weight - points[0].weight })) : [];
      const maxAbs = series.length ? Math.max(...series.map(point => Math.abs(point.drift))) : 0;
      const yLimit = Math.max(1, Math.ceil(maxAbs * 2.5 * 2) / 2);
      const latestAt = series.at(-1)?.at || Date.now();
      const earliestAt = latestAt - 60000;
      const x = at => plot.left + Math.max(0, Math.min(1, (at - earliestAt) / 60000)) * width;
      const y = value => plot.top + (yLimit - value) / (yLimit * 2) * height;

      context.fillStyle = '#e8f6ed';
      context.fillRect(plot.left, y(0.1), width, Math.max(2, y(-0.1) - y(0.1)));
      context.strokeStyle = '#d9e0ea';
      context.lineWidth = 1;
      context.font = '12px "Microsoft YaHei UI", sans-serif';
      context.fillStyle = '#78869c';
      context.textAlign = 'right';
      context.textBaseline = 'middle';
      for (let index = 0; index <= 4; index += 1) {
        const value = yLimit - (index / 4) * yLimit * 2;
        const py = plot.top + (index / 4) * height;
        context.beginPath(); context.moveTo(plot.left, py); context.lineTo(plot.right, py); context.stroke();
        context.fillText(value.toFixed(2), plot.left - 10, py);
      }
      context.textAlign = 'center';
      context.textBaseline = 'top';
      for (let seconds = 0; seconds <= 60; seconds += 10) {
        const px = plot.left + seconds / 60 * width;
        context.beginPath(); context.moveTo(px, plot.top); context.lineTo(px, plot.bottom); context.stroke();
        context.fillText(String(seconds), px, plot.bottom + 9);
      }
      context.save();
      context.setLineDash([5, 4]);
      context.strokeStyle = '#71819a';
      context.beginPath(); context.moveTo(plot.left, y(0)); context.lineTo(plot.right, y(0)); context.stroke();
      context.restore();
      if (series.length > 1) {
        context.strokeStyle = '#3b5ee8';
        context.lineWidth = 2;
        context.lineJoin = 'round';
        context.beginPath();
        series.forEach((point, index) => {
          const px = x(point.at), py = y(point.drift);
          if (index === 0) context.moveTo(px, py); else context.lineTo(px, py);
        });
        context.stroke();
      }
    };
    const observer = new ResizeObserver(draw);
    observer.observe(canvas);
    draw();
    return () => observer.disconnect();
  }, [points]);

  return <canvas ref={canvasRef} className="scale-chart" role="img" aria-label="最近六十秒零点漂移曲线" />;
}

function ScaleEventsPanel({ events, onClear }) {
  const [filter, setFilter] = useState('all');
  const [query, setQuery] = useState('');
  const [paused, setPaused] = useState(false);
  const frozenRef = useRef([]);
  if (!paused) frozenRef.current = events;
  const source = paused ? frozenRef.current : events;
  const shown = source.filter(event => {
    const matchesFilter = filter === 'all' || event.kind === filter;
    const haystack = `${event.type} ${event.note} ${event.weight ?? ''} ${event.raw ?? ''}`.toLowerCase();
    return matchesFilter && haystack.includes(query.toLowerCase());
  });
  return <section className="scale-events" aria-label="采样记录">
    <div className="trace__head">
      <h2 className="trace__title">采样记录</h2>
      <span className="chip chip--soft">称重事件 · 网页保留最近 120 条</span>
      <div className="trace__filters" role="group" aria-label="筛选称重记录">
        {[['all','全部'],['stable','稳定'],['drift','漂移'],['action','操作']].map(([value,label]) => <button type="button" key={value} className={`pill${filter===value?' is-active':''}`} aria-pressed={filter===value} onClick={()=>setFilter(value)}>{label}</button>)}
      </div>
      <label className="checkbox"><input type="checkbox" checked={paused} onChange={event=>setPaused(event.target.checked)}/><span>暂停显示</span></label>
      <div className="search search--compact"><GlyphSearch/><input className="search__input" type="search" value={query} placeholder="搜索采样 / 操作" aria-label="搜索称重记录" onChange={event=>setQuery(event.target.value)}/></div>
      <span className="trace__count">显示 {shown.length} / 共 {source.length} 条</span>
      <button type="button" className="link-button" onClick={onClear}>清空记录</button>
    </div>
    <div className="scale-events__table" role="table" aria-label="称重采样记录">
      <div className="scale-events__row scale-events__row--head" role="row"><span role="columnheader">时间</span><span role="columnheader">类型</span><span role="columnheader">净重 (g)</span><span role="columnheader">原始计数</span><span role="columnheader">状态</span><span role="columnheader">说明</span></div>
      <div className="scale-events__body">
        {shown.length ? shown.map(event => <div className="scale-events__row" role="row" key={event.id}>
          <span className="scale-events__time" role="cell">{formatTime(event.at)}</span><span role="cell">{event.type}</span><span className="scale-events__number" role="cell">{formatNumber(event.weight,3)}</span><span className="scale-events__number" role="cell">{formatInteger(event.raw)}</span><span className={`scale-events__state scale-events__state--${event.kind}`} role="cell">{event.state}</span><span className="scale-events__note" role="cell">{event.note}</span>
        </div>) : <p className="trace__empty">等待称重采样记录。</p>}
      </div>
    </div>
    <p className="trace__foot">两个全桥并联后由 HX711 读取为一个合成通道；漂移与噪声由网页根据最近 60 秒采样计算。</p>
  </section>;
}

export function ScaleWorkbench() {
  const [scale, setScale] = useState(EMPTY_SCALE);
  const [connected, setConnected] = useState(false);
  const [history, setHistory] = useState([]);
  const [events, setEvents] = useState([]);
  const [action, setAction] = useState('');
  const [message, setMessage] = useState('等待板端称重数据');
  const [calibrationOpen, setCalibrationOpen] = useState(false);
  const [knownWeight, setKnownWeight] = useState('500');
  const [pinConfigOpen, setPinConfigOpen] = useState(false);
  const [doutPinInput, setDoutPinInput] = useState('1');
  const [sckPinInput, setSckPinInput] = useState('2');
  const lastSampleRef = useRef(null);
  const lastEventAtRef = useRef(0);
  const wasTaringRef = useRef(false);
  const eventIdRef = useRef(0);

  const addEvent = event => setEvents(previous => [{ id: ++eventIdRef.current, at: Date.now(), ...event }, ...previous].slice(0, 120));

  useEffect(() => {
    let disposed = false, timer;
    const controller = new AbortController();
    const poll = async () => {
      try {
        const next = await request('/api/scale', undefined, controller.signal);
        if (disposed) return;
        setScale(next); setConnected(true);
        const now = Date.now();
        const firstSample = lastSampleRef.current == null;
        if (next.sampleCount !== lastSampleRef.current) {
          lastSampleRef.current = next.sampleCount;
          if (Number.isFinite(next.weightG)) setHistory(previous => [...previous, { at: now, weight: next.weightG, stable: next.stable, sampleCount: next.sampleCount }].filter(point => point.at >= now - 60000));
          if (now - lastEventAtRef.current >= 2000) {
            lastEventAtRef.current = now;
            addEvent({ kind: next.stable ? 'stable' : 'drift', type: '采样', weight: next.weightG, raw: next.rawCounts, state: STATUS_LABELS[next.status] || next.status, note: next.calibrated ? '读取合成通道净重' : '等待完成校准' });
          }
        }
        if (firstSample) setMessage('实时采集中');
        if (next.tareInProgress) { wasTaringRef.current = true; setMessage('正在采集去皮样本…'); }
        else if (wasTaringRef.current && next.tareCompleted) {
          wasTaringRef.current = false;
          setMessage('去皮完成，零点已更新');
          addEvent({ kind:'action', type:'去皮', weight:next.weightG, raw:next.rawCounts, state:'已完成', note:`零点偏移 ${formatInteger(next.tareRaw)}` });
        }
      } catch {
        if (!disposed) { setConnected(false); setScale(EMPTY_SCALE); setMessage('设备离线，称重读数已清空'); }
      }
      if (!disposed) timer = setTimeout(poll, 250);
    };
    poll();
    return () => { disposed = true; controller.abort(); clearTimeout(timer); };
  }, []);

  const diagnostics = useMemo(() => deriveDiagnostics(history, scale, connected), [history, scale, connected]);
  const calibrationWeight = Number(knownWeight);
  const calibrationValid = Number.isFinite(calibrationWeight) && calibrationWeight >= 1 && calibrationWeight <= 5000;
  const parsedDoutPin = Number(doutPinInput);
  const parsedSckPin = Number(sckPinInput);
  const pinAllowed = value => BASE_SCALE_PINS.has(value) || (value === 45 && scale.gpio45Allowed);
  const pinConfigValid = Number.isInteger(parsedDoutPin) && Number.isInteger(parsedSckPin) &&
    parsedDoutPin !== parsedSckPin && pinAllowed(parsedDoutPin) && pinAllowed(parsedSckPin);

  const runTare = async () => {
    if (action) return;
    setAction('tare'); setMessage('正在启动去皮…');
    try {
      await request('/api/scale/tare', {});
      wasTaringRef.current = true; setHistory([]); setMessage('正在采集去皮样本…');
      addEvent({ kind:'action', type:'去皮', weight:scale.weightG, raw:scale.rawCounts, state:'已启动', note:'板端开始采集 16 个零点样本' });
    } catch (error) { setMessage(SCALE_ERRORS[error.message] || error.message); }
    finally { setAction(''); }
  };

  const runCalibration = async () => {
    if (action || !calibrationValid) return;
    setAction('calibrate'); setMessage('正在写入校准系数…');
    try {
      const next = await request('/api/scale/calibrate', { knownWeightG: calibrationWeight });
      setScale(next); setCalibrationOpen(false); setMessage(`已使用 ${formatNumber(calibrationWeight,0)} g 完成校准`);
      addEvent({ kind:'action', type:'校准', weight:next.weightG, raw:next.rawCounts, state:'已完成', note:`标准砝码 ${formatNumber(calibrationWeight,0)} g；系数 ${formatNumber(next.countsPerGram,3)} counts/g` });
    } catch (error) { setMessage(SCALE_ERRORS[error.message] || error.message); }
    finally { setAction(''); }
  };

  const openPinConfig = () => {
    setDoutPinInput(Number.isInteger(scale.doutPin) ? String(scale.doutPin) : '1');
    setSckPinInput(Number.isInteger(scale.sckPin) ? String(scale.sckPin) : '2');
    setPinConfigOpen(true);
  };

  const runPinConfig = async () => {
    if (action || !pinConfigValid) return;
    setAction('pins'); setMessage('正在保存 HX711 IO 配置…');
    try {
      const next = await request('/api/scale/config', { doutPin: parsedDoutPin, sckPin: parsedSckPin });
      setScale(next); setPinConfigOpen(false); setHistory([]);
      setMessage(`HX711 已切换到 DOUT GPIO ${next.doutPin} / SCK GPIO ${next.sckPin}`);
      addEvent({ kind:'action', type:'IO 配置', weight:null, raw:null, state:'已保存', note:`DOUT GPIO ${next.doutPin}；SCK GPIO ${next.sckPin}` });
    } catch (error) { setMessage(SCALE_ERRORS[error.message] || error.message); }
    finally { setAction(''); }
  };

  return <>
    <main className="scale-workspace">
      <CompactPanel title="称重状态" initiallyOpen className="compact-panel--scale-summary"><aside className="scale-summary" aria-label="称重传感器状态">
        <div className="scale-summary__head"><h2>称重传感器</h2><span className={`chip ${scale.calibrated?'chip--ok':'chip--warn'}`}>{scale.calibrated?'已校准':'待校准'}</span></div>
        <p className="scale-summary__sub">HX711 · 通道 1<br/>两个全桥传感器并联</p>
        <p className="scale-summary__label">当前净重</p><p className="scale-summary__weight">{formatNumber(scale.weightG,2)} <small>g</small></p>
        <dl className="scale-summary__list"><div><dt>状态</dt><dd className={`scale-tone scale-tone--${diagnostics.tone}`}><GlyphDot tone={diagnostics.tone} size={10}/>{diagnostics.label}</dd></div><div><dt>设备状态</dt><dd>{connected && scale.initialized ? '在线' : '离线'}</dd></div><div><dt>数据更新时间</dt><dd>{Number.isFinite(scale.sampleAgeMs) ? `${scale.sampleAgeMs} ms` : '—'}</dd></div></dl>
        <div className="scale-summary__divider"/><h3>操作状态</h3><p className="scale-summary__message" role="status">{message}</p>
      </aside></CompactPanel>

      <CompactPanel title="零点漂移与校准" className="compact-panel--scale-drift"><section className="scale-drift" aria-label="零点漂移诊断">
        <div className="scale-drift__head"><h2>零点漂移诊断</h2><span className={`chip ${connected?'chip--ok':'chip--muted'}`}>{connected?'实时采集':'等待设备'}</span><span className="scale-drift__channel">HX711 通道 1（两个全桥并联）</span></div>
        <div className="scale-kpis"><div><span>当前净重</span><strong>{formatNumber(scale.weightG,2)} <small>g</small></strong></div><div><span>60 秒漂移量</span><strong>{Number.isFinite(diagnostics.drift) ? `${diagnostics.drift>=0?'+':''}${formatNumber(diagnostics.drift,2)}` : '—'} <small>g</small></strong></div><div><span>噪声幅度（峰-峰值）</span><strong>{formatNumber(diagnostics.noise,2)} <small>g</small></strong></div></div>
        <div className="scale-chart-wrap"><span className="scale-chart__axis">漂移 (g)</span><ScaleChart points={diagnostics.points}/><span className="scale-chart__time">时间（秒）</span></div>
        <div className="scale-chart__legend"><span><i className="legend-line"/>重量信号</span><span><i className="legend-band"/>稳定范围（±0.10 g）</span><span><i className="legend-zero"/>零点基线</span></div>
        <div className="scale-actions"><button type="button" className="button button--primary" disabled={!connected || !scale.initialized || action || scale.tareInProgress} onClick={runTare}>{scale.tareInProgress?'正在去皮…':'重新去皮'}</button><button type="button" className="button button--outline" disabled={!connected || !scale.initialized || action} onClick={()=>setCalibrationOpen(open=>!open)}>校准传感器</button><p><GlyphInfo/>请确保称重机构保持静止，避免振动和外力干扰。</p></div>
        {calibrationOpen && <div className="scale-calibration"><label htmlFor="known-weight">标准砝码重量</label><div><input id="known-weight" className={`input input--mono${knownWeight&&!calibrationValid?' has-error':''}`} value={knownWeight} inputMode="decimal" onChange={event=>setKnownWeight(event.target.value)}/><span>g</span></div><button type="button" className="button button--primary" disabled={!calibrationValid || action || !scale.tareCompleted} onClick={runCalibration}>{action==='calibrate'?'正在校准…':'使用此重量校准'}</button><button type="button" className="link-button" onClick={()=>setCalibrationOpen(false)}>取消</button>{!scale.tareCompleted && <span className="scale-calibration__hint">校准前必须先完成去皮</span>}</div>}
      </section></CompactPanel>

      <CompactPanel title="诊断与 IO 设置" desktopInitiallyOpen={false} className="compact-panel--scale-diagnostics"><aside className="scale-diagnostics" aria-label="诊断信息">
        <div className="scale-diagnostics__head"><h2>诊断信息</h2><button type="button" className="link-button" disabled={!connected || action} onClick={openPinConfig}>配置 IO</button></div>
        {pinConfigOpen && <div className="scale-pin-config" aria-label="HX711 IO 配置">
          <label htmlFor="scale-dout-pin">DOUT GPIO</label><input id="scale-dout-pin" className={`input input--mono${doutPinInput&&!pinAllowed(parsedDoutPin)?' has-error':''}`} value={doutPinInput} inputMode="numeric" onChange={event=>setDoutPinInput(event.target.value)}/>
          <label htmlFor="scale-sck-pin">SCK GPIO</label><input id="scale-sck-pin" className={`input input--mono${sckPinInput&&!pinAllowed(parsedSckPin)?' has-error':''}`} value={sckPinInput} inputMode="numeric" onChange={event=>setSckPinInput(event.target.value)}/>
          <div className="scale-pin-config__actions"><button type="button" className="button button--primary" disabled={!pinConfigValid || action} onClick={runPinConfig}>{action==='pins'?'正在保存…':'保存并应用'}</button><button type="button" className="link-button" onClick={()=>setPinConfigOpen(false)}>取消</button></div>
          <p>可用 GPIO：1、2、6–18、21、38–42{scale.gpio45Allowed?'、45':''}、47、48。GPIO45 仅在 eFuse 已把 VDD_SPI 固定为 3.3 V 时开放。</p>
        </div>}
        <dl><div><dt>HX711 DOUT</dt><dd>{Number.isInteger(scale.doutPin)?`GPIO ${scale.doutPin}`:'—'}</dd></div><div><dt>HX711 SCK</dt><dd>{Number.isInteger(scale.sckPin)?`GPIO ${scale.sckPin}`:'—'}</dd></div><div><dt>原始计数</dt><dd>{formatInteger(scale.rawCounts)}</dd></div><div><dt>净重计数</dt><dd>{formatInteger(scale.netCounts)}</dd></div><div><dt>采样频率</dt><dd>{Number.isFinite(diagnostics.sampleRate)?`${formatNumber(diagnostics.sampleRate,1)} Hz`:'—'}</dd></div><div><dt>样本更新时间</dt><dd>{Number.isFinite(scale.sampleAgeMs)?`${scale.sampleAgeMs} ms`:'—'}</dd></div><div><dt>稳定持续时间</dt><dd>{`${formatNumber(diagnostics.stableSeconds,1)} s`}</dd></div><div><dt>零点偏移</dt><dd>{formatInteger(scale.tareRaw)}</dd></div><div><dt>每克计数</dt><dd>{formatNumber(scale.countsPerGram,3)}</dd></div><div><dt>故障状态</dt><dd className={scale.status==='fault'||scale.status==='stale'?'is-danger':'is-ok'}>{scale.status==='fault'||scale.status==='stale'?(STATUS_LABELS[scale.status]||scale.status):'无故障'}</dd></div></dl><p className="scale-diagnostics__note"><GlyphInfo/>两个全桥并联读作一个合成通道；无法分别判断单个传感器漂移。</p>
      </aside></CompactPanel>
    </main>
    <CompactPanel title="称重采样记录" desktopInitiallyOpen={false} className="compact-panel--scale-events"><ScaleEventsPanel events={events} onClear={()=>setEvents([])}/></CompactPanel>
  </>;
}
