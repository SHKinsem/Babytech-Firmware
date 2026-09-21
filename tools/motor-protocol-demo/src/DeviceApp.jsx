import { useEffect, useRef, useState } from 'react';
import { CommandLibrary } from './components/CommandLibrary.jsx';
import { CommandPanel } from './components/CommandPanel.jsx';
import { ManualPanel } from './components/ManualPanel.jsx';
import { TracePanel } from './components/TracePanel.jsx';
import { WifiPanel } from './components/WifiPanel.jsx';
import { COMMAND_GROUPS, getCommandItem, defaultValues, encodeCommand, buildFrames, frameAnnotation, formatBytes, formatCanId, parseLogicalHex, identifyCommandBytes, validateAddress } from './protocol.js';
import { planManualMove, MANUAL_DEFAULTS } from './simulation.js';
import { request, supportReason, stateLabels, errorLabels } from './device-api.js';

const EMPTY = {id:0, enabled:false, online:false, positionDeg:null, speedRpm:null, currentMa:null};
const cleanItem = original => ({...original, note: null, variants:original.variants.map(v => ({...v,layout:v.layout.map(f => f.key === 'sync' ? {...f,hint:'实机只接受立即执行；同步选项用于组帧预览。'} : f)}))});
const metric = (value, unit) => value == null ? '—' : `${value} ${unit}`;

function DeviceFeedback({ status, connected, notice }) {
  return <section className="panel panel--feedback" aria-label="电机反馈">
    <div className="feedback__head"><h2 className="panel__title">电机反馈</h2><span className={`chip ${status.enabled && connected ? 'chip--ok' : 'chip--muted'}`}>{connected ? status.enabled ? '使能已确认' : '使能未确认' : '反馈未知'}</span></div>
    <p className="feedback__address">电机 {status.id || '—'} · {connected && status.online ? '实时反馈' : '等待新鲜反馈'}</p>
    <dl className="metrics"><div className="metrics__row"><dt>位置</dt><dd>{metric(status.positionDeg,'°')}</dd></div><div className="metrics__row"><dt>速度</dt><dd>{metric(status.speedRpm,'RPM')}</dd></div><div className="metrics__row"><dt>电流</dt><dd>{metric(status.currentMa,'mA')}</dd></div></dl>
    <div className="divider"/><h3 className="section__title">控制状态</h3>
    <p>{connected ? stateLabels[status.state] || status.state || '读取中' : '设备离线，读数已清空'}</p>
    <p>CAN：{status.busState || '—'} · TX 错误 {status.txErrors ?? '—'}</p>
    <p>驱动实际使能：{status.driverEnabled == null ? '未知' : status.driverEnabled ? '开' : '关'}</p>
    <p>最近控制应答：{status.lastAck || '—'}</p>
    {status.fault && status.fault !== 'none' && <p className="device-error" role="alert">{errorLabels[status.fault] || status.fault}</p>}
    <div className="divider"/><h3 className="section__title">请求结果</h3><p className="device-notice" role="status">{notice || '尚未提交操作'}</p>
    <p className="capabilities__note">HTTP 202 表示指令已入队。使能及停止以真实应答／反馈确认，未知应答仅保留原始字节。</p>
    <div className="divider"/><h3 className="section__title">试验边界</h3><p className="capabilities__note">速度／力矩最多运行 5 秒，板端自动停止；反馈超时提前停止。同步、直通位置、FD 与回零执行目前仅可预览。参数写入需要驱动关闭使能且静止。</p>
  </section>;
}

export function DeviceApp() {
  useEffect(() => {document.body.classList.add('device-body');return () => document.body.classList.remove('device-body');},[]);
  const [draft,setDraft] = useState('1'), [tab,setTab] = useState('manual');
  const check = validateAddress(draft), address = check.ok ? check.value : 0;
  const [status,setStatus] = useState(EMPTY), [connected,setConnected] = useState(false), [notice,setNotice] = useState('');
  const [busy,setBusy] = useState(false), busyRef = useRef(false);
  const [selected,setSelected] = useState('enable'), [variant,setVariant] = useState('base');
  const [values,setValues] = useState(() => defaultValues(getCommandItem('enable'),'base'));
  const [mode,setMode] = useState('form'), [raw,setRaw] = useState(''), [dirty,setDirty] = useState(false);
  const [query,setQuery] = useState(''), [groups,setGroups] = useState(() => Object.fromEntries(COMMAND_GROUPS.map(g => [g.id,!!g.open])));
  const [manual,setManual] = useState({...MANUAL_DEFAULTS, angle:'10',speed:'10',current:'300'});
  const [records,setRecords] = useState([]), [filter,setFilter] = useState('all'), [traceQuery,setTraceQuery] = useState(''), [frozen,setFrozen] = useState(null);
  const targetRef = useRef(address); targetRef.current = address;
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
  const needsEnable = [0xcd,0xf5,0xc5,0xf6,0xc6].includes(model.bytes[1]);
  const gate = !address ? check.error : !connected ? '等待设备连接' : !current.canReady ? 'CAN 控制器不可用' : busy ? '等待当前请求返回' : !model.ok ? '请修正参数' : supportReason(model.bytes) || (needsEnable && !current.enabled ? '请先发送使能，并等待真实确认' : null);

  useEffect(() => {
    let disposed = false, timer;
    const controller = new AbortController();
    setStatus({...EMPTY,id:address}); setConnected(false);
    async function poll() {
      try {
        if (!address) return;
        const next = await request(`/api/status?id=${address}`,undefined,controller.signal);
        if (!disposed && targetRef.current === address) {setStatus(next);setConnected(true);}
      } catch { if (!disposed) {setConnected(false);setStatus({...EMPTY,id:address});} }
      if (!disposed) timer = setTimeout(poll,300);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[address]);

  useEffect(() => {
    let disposed = false, timer, seq = 0, uptime = 0, generation = 0;
    const controller = new AbortController();
    async function poll() {
      try {
        const data = await request('/api/trace',undefined,controller.signal);
        if (disposed) return;
        if (data.uptimeMs < uptime || data.sequence < seq) {seq=0;generation++;}
        uptime = data.uptimeMs;
        const incoming = data.frames.filter(f => f.seq > seq);
        const lost = incoming.length && seq && incoming[0].seq > seq+1;
        if (lost) setNotice('部分总线记录已被环形缓冲区覆盖；以下仅为保留记录。');
        const rows = incoming.map(f => ({id:`${generation}-${f.seq}`,at:Date.now()-(data.uptimeMs-f.atMs),dir:f.dir,canId:f.id,dlc:f.data.length,data:f.data,note:`${f.extended ? '扩展帧' : '标准帧'}${f.remote ? ' · 远程帧' : ''} · ${f.dir === 'TX' ? 'TWAI 已入队' : '总线实际接收'}`}));
        seq=data.sequence;
        if (rows.length) setRecords(prev => [...prev,...rows].slice(-400));
      } catch { /* Status poll owns connection state. Never fabricate RX. */ }
      if (!disposed) timer=setTimeout(poll,500);
    }
    poll(); return () => {disposed=true;controller.abort();clearTimeout(timer);};
  },[]);

  async function submit(path,data,{stop=false}={}) {
    if (!stop && busyRef.current) return;
    if (!stop) {busyRef.current=true;setBusy(true);}
    const id = data.id ?? address;
    try {
      const result = await request(path,data);
      setNotice(`电机 ${stop && path.endsWith('stop-all') ? '全部' : id}：${result.message === 'queued_auto_stop_5s' ? '已入队，板端最多运行 5 秒' : '请求已入队，等待真实反馈'}`);
    } catch (e) {setNotice(`${errorLabels[e.message] || e.message}。未自动重试；请求超时不代表设备未执行。`);}
    finally {if (!stop) {busyRef.current=false;setBusy(false);}}
  }
  function choose(id) {const next=getCommandItem(id);setSelected(id);setVariant(next.variants[0].key);setValues(defaultValues(next,next.variants[0].key));setDirty(false);setMode('form');}
  async function copy(text) {
    try {
      if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text);
      else {const el=document.createElement('textarea');el.value=text;document.body.appendChild(el);el.select();const ok=document.execCommand('copy');el.remove();if (!ok) throw Error();}
      setNotice('已复制');
    } catch {setNotice('复制失败，请手动选择报文。');}
  }
  const prediction=planManualMove(manual);
  const moveEncoded=prediction.ok ? encodeCommand({item:getCommandItem('position'),variantKey:'limit',values:prediction.plan,address:address || 1}) : {bytes:[],labels:[]};
  const moveFrames=buildFrames(moveEncoded.bytes || []);
  const manualGate=!address ? check.error : !connected ? '等待设备连接' : !current.canReady ? 'CAN 控制器不可用' : busy ? '等待请求返回' : !prediction.ok ? Object.values(prediction.errors)[0] : !current.enabled ? '请先发送使能，并等待确认' : !current.online ? '等待新鲜电机反馈' : current.fault && current.fault !== 'none' ? '故障未清除，请停止后重新使能' : current.activeId || current.state === 'moving' || current.state === 'experiment_running' || current.state === 'stop_requested' ? '电机忙，请等待停止' : null;
  const shown=(frozen ?? records).filter(r => (filter==='all'||r.dir===filter) && `${formatBytes(r.data)} ${formatCanId(r.canId)} ${r.note}`.toLowerCase().includes(traceQuery.toLowerCase()));

  return <div className="app device-app">
    <header className="toolbar"><div className="toolbar__brand"><span className="toolbar__logo">Babytech</span><span className="toolbar__divider">/</span><h1 className="toolbar__title">电机协议工作台</h1></div>
      <div className="toolbar__right"><span className="chip chip--soft">实机模式</span><label className="toolbar__field">CAN ID <input aria-label="CAN ID" className="input input--mono toolbar__address" value={draft} onChange={e=>setDraft(e.target.value)} inputMode="numeric" aria-invalid={!check.ok}/></label><span>CAN 500 kbit/s</span><span className={`chip ${connected?'chip--ok':'chip--muted'}`}>{connected?'设备在线':'设备未连接'}</span><button className="button button--danger" onClick={()=>submit('/api/stop-all',{}, {stop:true})}>全部停止</button></div></header>
    <nav className="tabs" role="tablist" aria-label="工作模式">{[['manual','常规试动'],['lab','指令实验室'],['wifi','Wi-Fi 设置']].map(([id,label])=><button key={id} role="tab" aria-selected={tab===id} className={`tabs__item${tab===id?' is-active':''}`} onClick={()=>setTab(id)}>{label}</button>)}<span className="device-network-note">板端真实接口 · 不自动重发操作</span></nav>
    {tab==='wifi' ? <WifiPanel notify={setNotice}/> : <main className={`workspace workspace--${tab}`}>
      {tab==='lab' ? <><CommandLibrary query={query} onQueryChange={setQuery} openGroups={groups} onToggleGroup={id=>setGroups(g=>({...g,[id]:!g[id]}))} selectedId={selected} onSelect={choose}/>
      <CommandPanel device item={item} variantKey={variant} onVariantChange={key=>{setVariant(key);setValues(defaultValues(item,key));setDirty(false);}} values={values} onValueChange={(key,value)=>setValues(v=>({...v,[key]:value}))} editorMode={mode} onEditorModeChange={m=>{setMode(m);setRaw(formatBytes(form.bytes || []));setDirty(false);}} rawText={dirty?raw:formatBytes(form.bytes || [])} onRawTextChange={text=>{setRaw(text);setDirty(true);}} onRawTextReplace={text=>{setRaw(text);setDirty(true);}} rawDirty={dirty} onResetRaw={()=>setDirty(false)} identifiedItem={model.identified?model.item:null} model={model} frames={frames} annotations={annotations} address={address || 1} addressError={check.ok?null:check.error} gateReason={gate} onSend={()=>{if(!gate)submit('/api/command',{hex:formatBytes(model.bytes).replaceAll(' ','')});}} onCopy={copy}/></> : <ManualPanel device values={manual} errors={prediction.errors} onChange={(key,value)=>setManual(v=>({...v,[key]:value}))} motor={{enabled:current.enabled,positionTenths:current.positionDeg==null?null:current.positionDeg*10}} address={address || 1} prediction={prediction} bytes={moveEncoded.bytes || []} frames={moveFrames} annotations={moveFrames.map(f=>frameAnnotation(moveEncoded.bytes,moveEncoded.labels,f))} gateReason={manualGate} onSend={()=>{if(!manualGate)submit('/api/move',{id:address,angle:Number(manual.angle)*(Number(manual.dir)===1?-1:1),speed:manual.speed,accel:manual.accel,decel:manual.decel,current:manual.current});}} onStop={()=>{if(address)submit('/api/stop',{id:address},{stop:true});}} onCopy={copy} onGoToEnable={()=>{setTab('lab');choose('enable');}}/>}
      <DeviceFeedback status={current} connected={connected} notice={notice}/>
    </main>}
    <TracePanel device records={shown} totalCount={records.length} hiddenCount={0} filter={filter} onFilterChange={setFilter} query={traceQuery} onQueryChange={setTraceQuery} paused={frozen!==null} onTogglePause={()=>setFrozen(frozen?null:[...records])} onClear={()=>{setRecords([]);if(frozen)setFrozen([]);}} onCopy={copy}/>
    {tab==='wifi' && notice && <div className="toast" role="status">{notice}</div>}
  </div>;
}
