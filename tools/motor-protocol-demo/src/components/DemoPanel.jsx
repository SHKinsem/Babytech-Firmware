import { useEffect, useRef, useState } from 'react';
import { request } from '../device-api.js';
import './demo-panel.css';

const stages = [['initialization','初始化找零'],['open_cap','开盖'],['water','加水'],['powder','加粉'],['close_cap','关盖'],['mix','混合']];
const stageNames = {noready:'未就绪',ready:'就绪',unscrewing_cap:'开盖',dispensing_water:'加水',dispensing_powder:'加粉',screwing_cap:'关盖',mixing:'混合',complete:'完成',error:'故障'};
const draftKey = 'babytech.demo-json.v1';
const readDraft = () => { try { return localStorage.getItem(draftKey) || ''; } catch { return ''; } };
const canonical = text => { try { return JSON.stringify(JSON.parse(text)); } catch { return null; } };

export function DemoPanel({unverifiedMode = false}) {
  const [draft,setDraft] = useState(readDraft);
  const [applied,setApplied] = useState('');
  const [selected,setSelected] = useState('initialization');
  const [status,setStatus] = useState(null);
  const [online,setOnline] = useState(false);
  const [pending,setPending] = useState(false);
  const [message,setMessage] = useState('');
  const generation = useRef(0);
  const edited = useRef(Boolean(draft));
  const alive = useRef(true);
  useEffect(() => {
    alive.current = true;
    const controller = new AbortController();
    let timer;
    async function poll() {
      const at = generation.current;
      try {
        const next = await request('/api/demo',undefined,controller.signal);
        if (alive.current && at === generation.current) { setStatus(next); setOnline(true); }
      } catch { if (alive.current && at === generation.current) setOnline(false); }
      if (alive.current) timer = setTimeout(poll,700);
    }
    poll();
    const configGeneration = generation.current;
    request('/api/demo/config',undefined,controller.signal).then(config => {
      if (!alive.current || configGeneration !== generation.current) return;
      const text = JSON.stringify(config,null,2);
      setApplied(text);
      if (!edited.current) setDraft(text);
    }).catch(() => {});
    return () => { alive.current = false; controller.abort(); clearTimeout(timer); };
  },[]);
  function edit(text) {
    edited.current = true; setDraft(text);
    try { localStorage.setItem(draftKey,text); } catch { /* RAM draft still usable */ }
  }
  let config = null;
  try { config = JSON.parse(draft); } catch { /* keep incomplete JSON editable */ }
  const script = selected === 'initialization' ? config?.initialization : config?.stages?.find(s=>s.id===selected);
  const dirty = canonical(draft) === null || canonical(draft) !== canonical(applied);
  const locked = pending || !online || !status?.available || status?.busy;
  async function post(path,data,appliedText) {
    if (pending) return;
    setPending(true); ++generation.current;
    try {
      const next = await request(path,data);
      if (!alive.current) return;
      setStatus(next); setOnline(true);
      if (appliedText !== undefined) setApplied(appliedText);
      setMessage(appliedText !== undefined ? '配置已应用到 RAM；未启动电机。' : unverifiedMode
        ? '请求已接受；不校验模式只报告指令提交，实际运动状态请自行核对。'
        : '请求已接受，请观察板端状态。');
    } catch (error) {
      if (!alive.current) return;
      setMessage(error.uncertain ? '请求结果未知；未自动重发。请核对板端状态或使用顶部“全部停止”。' : `板端拒绝：${error.message}`);
      if (error.uncertain) setOnline(false);
    } finally {
      ++generation.current;
      if (alive.current) setPending(false);
    }
  }
  function updateScript(commands) {
    if (!config || !script) return;
    script.commands = commands.split('\n');
    edit(JSON.stringify(config,null,2));
  }
  async function load(event) {
    const file = event.target.files?.[0];
    event.target.value = '';
    if (!file) return;
    if (file.size > 16384) { setMessage('JSON 文件不能超过 16384 bytes。'); return; }
    const text = await file.text();
    if (!alive.current) return;
    if (!canonical(text)) { setMessage('JSON 格式无效，原草稿已保留。'); return; }
    edit(text); setMessage('已载入本机编辑器，尚未应用；不会触发运动。');
  }
  function exportJson() {
    if (!config) return;
    const url = URL.createObjectURL(new Blob([JSON.stringify(config,null,2)+'\n'],{type:'application/json'}));
    const a = document.createElement('a'); a.href=url; a.download='demo_flow.json'; a.click();
    setTimeout(()=>URL.revokeObjectURL(url),1000);
  }
  return <main className="demo-workbench" aria-label="屏幕流程演示">
    <section className="panel demo-status">
      <h2 className="panel__title">屏幕流程演示</h2>
      <p>仅供演示，产物不得用于喂养。水量与温度为配置值。</p>
      <p role="status">{!online ? '状态未连接' : !status?.available ? '当前为 BRAIN 固件，请编译 DISPLAY 分支。' : status.initializing ? unverifiedMode ? '正在发送初始化脚本' : '正在初始化找零' : unverifiedMode && status.stage==='complete' ? '脚本发送结束（实际运动未知）' : stageNames[status.stage] || status.stage}</p>
      <dl><dt>软件参考</dt><dd>{online && status?.referenceValid ? '有效' : '无有效确认'}</dd><dt>流程配置</dt><dd>{status?.configured ? '已配置' : '需要填写脚本和轴配置'}</dd><dt>板端原因</dt><dd>{status?.reason || '—'}</dd><dt>Error</dt><dd>{status?.error || '—'}</dd></dl>
      <button className="button button--outline" disabled={locked || dirty || !status?.configured} onClick={()=>post('/api/demo/action',{action:'initialize'})}>复位 / 初始化</button>
      <button className="button button--primary" disabled={locked || dirty || (unverifiedMode ? !status?.configured : !status?.startEnabled)} onClick={()=>post('/api/demo/action',{action:'start'})}>完整流程运行</button>
      <p>{unverifiedMode ? '不校验模式按脚本提交可编码指令，不等待零点或到位确认；“发送结束”不证明机械完成。停止请使用顶部“全部停止”。' : '上电不运动。首次点击初始化才找零；后续从屏幕 Start 启动。停止请使用顶部“全部停止”。'}</p>
      <p>开盖 → 加水 → 加粉 → 关盖 → 混合（先回软件零点）→ Complete 3 秒 → Ready</p>
      {message && <p className="device-notice" role="alert">{message}</p>}
    </section>
    <section className="panel demo-script">
      <h2 className="panel__title">阶段脚本</h2>
      <label>阶段<select className="input" value={selected} onChange={e=>setSelected(e.target.value)}>{stages.map(([id,label])=><option value={id} key={id}>{label}</option>)}</select></label>
      <textarea aria-label="阶段脚本" spellCheck={false} value={script?.commands?.join('\n') || ''} disabled={!script || pending} onChange={e=>updateScript(e.target.value)}/>
      <button className="button button--outline" disabled={locked || dirty || !script || selected==='initialization' || (!unverifiedMode && (!status?.referenceValid || status?.stage==='error'))} onClick={()=>post('/api/demo/action',{action:'stage',stage:selected})}>运行所选阶段</button>
      <p>{unverifiedMode ? '脚本修改保留在右侧 JSON，Apply 后才能运行。move/home 的 await 不等待运动反馈；wait 毫秒数仍按脚本计时。' : '脚本修改保留在右侧 JSON，Apply 后才能运行。move/home 必须显式 await；加水、加粉可以填写 wait 毫秒数进行模拟。'}</p>
      <p>混合脚本内使用 <code>zero ID RPM ACCEL DECEL CURRENT</code> 回到本次初始化记录的软件零点，再执行混合。初始化采用 <code>home ID 2 await</code>，碰撞参数须提前在驱动器上确认。</p>
    </section>
    <section className="panel demo-json">
      <h2 className="panel__title">JSON 配置</h2>
      <div className="demo-actions"><label className="button button--outline">Load JSON<input type="file" accept=".json,application/json" hidden onChange={load}/></label><button className="button button--outline" disabled={!config} onClick={exportJson}>Export JSON</button><button className="button button--primary" disabled={locked || !config} onClick={()=>post('/api/demo/config',{json:draft},draft)}>Apply</button></div>
      <textarea aria-label="演示 JSON" spellCheck={false} value={draft} onChange={e=>edit(e.target.value)} disabled={pending}/>
      <p>{dirty ? '本机草稿，尚未应用。' : '当前编辑内容与本页最近确认的 RAM 配置一致。'} 轮询不覆盖草稿。</p>
      <p>Apply 不触发运动。RAM 配置断电丢失；最终导出到 device-controller/data/demo_flow.json 后重新编译烧录。</p>
    </section>
  </main>;
}
