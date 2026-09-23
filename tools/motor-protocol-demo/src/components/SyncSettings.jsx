import { useEffect, useState } from 'react';
import { request, syncIsolationReasonText } from '../device-api.js';

const budgetFields=[['queriesPerSecond','总查询上限 / 秒'],['gapMs','最小间距 ms'],['timeoutMs','请求超时 ms'],['cooldownMs','超时冷却 ms'],['maxInflight','最大在途数']];
const syncFields=[['progressTolerance','进度容差 0..0.25'],['timeToleranceMs','量化时间容差 ms'],['feedbackTimeoutMs','反馈失效 ms'],['prepareTimeoutMs','准备期限 ms'],['stopTimeoutMs','停止确认期限 ms'],['responseBudgetMs','实测应答延迟预算 ms'],['completionTenths','到位角度容差 0.1°']];
const attestLabels=['已实测当前驱动的缓存覆盖、目标读回、停止及广播消费行为','已隔离所有非成员，确认全总线没有遗留同步缓存','已排空此前控制应答，当前驱动状态与台架记录一致'];

export function SyncSettings({ connected, locked }) {
  const [budget,setBudget]=useState(null),[sync,setSync]=useState(null);
  const [stats,setStats]=useState(null),[notice,setNotice]=useState('');
  const [pending,setPending]=useState(false),[attest,setAttest]=useState([false,false,false]);
  useEffect(()=>{
    if(!connected) return;
    let alive=true;
    Promise.all([request('/api/query-budget'),request('/api/sync-settings')]).then(([b,s])=>{
      if(!budgetFields.every(([k])=>Number.isFinite(b?.[k])) ||
         !syncFields.every(([k])=>Number.isFinite(s?.[k])) || typeof s?.cacheIsolationReady!=='boolean')
        throw new Error('板端未返回完整配置（请检查固件版本）');
      if(alive) {setBudget(b);setStats(b);setSync(s);}
    }).catch(e=>{if(alive) setNotice(`配置读取失败：${e.message}`);});
    return ()=>{alive=false;};
  },[connected]);
  async function save(path,data,kind) {
    if(pending) return;
    setPending(true);
    try {
      const result=await request(path,data);
      if(kind==='budget') {setBudget(result);setStats(result);}
      else {setSync(result);setAttest([false,false,false]);}
      setNotice('板端已确认保存；未发送运动指令。');
    } catch(e) {setNotice(e.status?`板端拒绝（HTTP ${e.status}）：${e.message}`:`操作结果未知：${e.message}。不会自动重发，请重新读取核对。`);}
    finally {setPending(false);}
  }
  async function refreshIsolation() {
    if(pending) return;
    setPending(true);
    try {
      const result=await request('/api/sync-settings');
      if(typeof result?.cacheIsolationReady!=='boolean') throw new Error('板端未返回启动许可状态');
      setSync(result);
      setNotice(result.cacheIsolationReady?'当前隔离确认仍有效。':'当前隔离确认无效；请先完成使能，再核实并记录确认。');
    } catch(e) {setNotice(`启动许可读取失败：${e.message}`);}
    finally {setPending(false);}
  }
  const fields=(items,value,setter)=>items.map(([key,label])=><label className="queue-builder__arg" key={key}>
    <span>{label}</span><input className="input input--mono" aria-label={label} value={value?.[key]??''}
      disabled={pending} onChange={e=>setter(prev=>({...prev,[key]:e.target.value}))}/>
  </label>);
  return <details className="sync-settings"><summary>查询预算与同步配置</summary>
    <p>显式保存到板端。这里的数值不是驱动吞吐能力证明；先空载测量，再配置速度、容差和延迟预算。</p>
    <div className="queue-builder__args">{fields(budgetFields,budget,setBudget)}</div>
    <button className="button button--outline" disabled={!budget||!connected||locked||pending}
      onClick={()=>save('/api/query-budget',Object.fromEntries(budgetFields.map(([k])=>[k,budget[k]])),'budget')}>保存全局查询预算</button>
    <div className="queue-builder__args">{fields(syncFields,sync,setSync)}</div>
    <button className="button button--outline" disabled={!sync||!connected||locked||pending}
      onClick={()=>save('/api/sync-settings',Object.fromEntries(syncFields.map(([k])=>[k,sync[k]])),'sync')}>保存同步容差与期限</button>
    <details><summary>启动许可（最后读取）：{sync?.cacheIsolationReady?'人工已确认隔离':'未确认／已失效'}</summary>
      <p>先在队列外完成使能，再核实总线并记录隔离确认；不要把 enable 与 sync 写在同一段程序里。当前固件的使能／失能、原始控制帧、重启或同步中断会使确认失效。本按钮不能清除或验证硬件缓存。</p>
      <p>板端记录原因：{sync?.cacheIsolationReason?syncIsolationReasonText(sync.cacheIsolationReason):'当前固件未提供'}。</p>
      <button className="link-button" disabled={!connected||pending} onClick={refreshIsolation}>刷新启动许可</button>
      {attestLabels.map((label,i)=><label className="queue-builder__check" key={label}>
        <input type="checkbox" checked={attest[i]} disabled={pending} onChange={e=>setAttest(a=>a.map((v,j)=>j===i?e.target.checked:v))}/>{label}
      </label>)}
      <button className="button button--outline" disabled={!connected||locked||pending||!attest.every(Boolean)}
        onClick={()=>save('/api/sync-isolation',{cacheSemanticsVerified:1,allNodesIsolated:1,pendingRepliesDrained:1},'sync')}>记录本次人工隔离确认</button>
      <button className="link-button" disabled={!connected||locked||pending}
        onClick={()=>save('/api/sync-isolation',{revoke:1},'sync')}>撤销许可</button>
    </details>
    <details><summary>总线统计（只读缓存）</summary>
      <button className="link-button" disabled={!connected||pending} onClick={async()=>{
        try {setStats(await request('/api/query-budget'));}catch(e){setNotice(`统计读取失败：${e.message}`);}
      }}>刷新统计</button>
      <p>驱动内部丢包、未知设备流量未观测，不代表零。帧数不等于驱动负载。</p>
      <pre className="sync-settings__stats">{stats?JSON.stringify(stats,null,2):'未读取'}</pre>
    </details>
    {notice?<p className="queue-notice queue-notice--warn" role="status">{notice}</p>:null}
  </details>;
}
