import { useEffect, useState } from 'react';
import { request } from '../device-api.js';

const labels = {
  config_wait_ack:'配置已提交，等待 4C 应答',
  config_wait_readback:'驱动器已接受，等待 22 读回核对',
  config_verified:'回零参数已读回，全部一致',
  config_rejected:'驱动器拒绝配置',
  config_ack_timeout:'4C 应答超时：结果未知，未自动重发',
  config_readback_timeout:'配置已接受，但 22 读回超时：尚未核对',
  config_mismatch:'读回参数与提交值不一致',
  config_read_tx_failed:'配置已接受，读回查询发送失败',
  config_cancelled:'配置核对已取消，已发送的配置可能已生效',
};
const fields = [
  ['回零模式',0,1,''],['方向',1,1,''],['回零速度',2,2,' RPM'],
  ['回零超时',4,4,' ms'],['碰撞转速阈值',8,2,' RPM'],
  ['碰撞电流阈值',10,2,' mA'],['碰撞持续时间',12,2,' ms'],['上电回零',14,1,''],
];
const value = (bytes,start,count) => bytes.slice(start,start+count).reduce((n,b)=>n*256+b,0);

// Separate persistent transaction evidence: feedback/trace polling cannot replace
// it with another opcode's ACK. Keep the last result visibly stale on disconnect.
export function ConfigResult() {
  const [result,setResult]=useState(null), [stale,setStale]=useState(false);
  useEffect(()=>{
    let disposed=false,timer;
    const controller=new AbortController();
    async function poll() {
      try {
        const next=await request('/api/config-result',undefined,controller.signal);
        if(!disposed && Number.isInteger(next.sequence) && next.sequence>0 && labels[next.state]) {
          setResult(next);setStale(false);
        } else if(!disposed && next.sequence===0) {setResult(null);setStale(false);}
      } catch {if(!disposed)setStale(true);}
      if(!disposed)timer=setTimeout(poll,800);
    }
    poll();return()=>{disposed=true;controller.abort();clearTimeout(timer);};
  },[]);
  if(!result)return null;
  const expected=Array.isArray(result.expected)?result.expected:[];
  const actual=Array.isArray(result.actual)?result.actual:[];
  return <section className="config-result" aria-label="回零参数设置结果" aria-live="polite">
    <strong>电机 {result.id} · 配置 #{result.sequence}：{labels[result.state]}</strong>
    {stale && <span>（连接中断，保留最后一次结果）</span>}
    {result.ack>0 && <span> · 4C 应答 0x{result.ack.toString(16).toUpperCase().padStart(2,'0')}</span>}
    <details><summary>查看提交值与读回值</summary>
      <table><thead><tr><th>参数</th><th>提交</th><th>读回</th></tr></thead><tbody>
        {fields.map(([name,start,count,unit])=>{
          const sent=expected.length===15?value(expected,start,count):null;
          const read=actual.length===15?value(actual,start,count):null;
          return <tr key={name}><td>{name}</td><td>{sent==null?'—':`${sent}${unit}`}</td>
            <td>{read==null?'待核对':`${read}${unit}${read!==sent?'（不一致）':''}`}</td></tr>;
        })}
      </tbody></table>
    </details>
  </section>;
}
