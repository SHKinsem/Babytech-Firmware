import { useState } from 'react';
import { queueMessageText } from '../device-api.js';
const labels = {
  raw_submitted: '原始帧已提交（不关联动作应答）',
  driver_no_motion_reported: '驱动报告无需回零运动',
  submitted: '已提交发送', receive_unconfirmed: '接收未确认', driver_accepted: '驱动接受',
  driver_completion_reported: '驱动报告完成（未确认机械到位）', driver_rejected: '驱动拒绝',
  association_uncertain: '应答关联不确定', send_failed: '发送失败',
};
const hex = (value) => Number.isInteger(value) ? `0x${value.toString(16).toUpperCase()}` : '—';

export function QueueDiagnostics({ queue, editorRef, notice }) {
  const [locationNote,setLocationNote]=useState('');
  const alert = queue?.alert;
  const events = queue?.diagnostics ?? [];
  function locate(line,runId=queue?.runId) {
    const editor = editorRef.current;
    if (!editor || !Number.isInteger(line) || line < 1) return;
    let hash=2166136261;
    for(const b of new TextEncoder().encode(editor.value)) hash=Math.imul(hash^b,16777619)>>>0;
    if(runId!==queue?.runId || hash!==queue?.programHash) {
      setLocationNote('当前草稿与该运行的源程序不一致或无法核对，请恢复对应程序后再定位。');return;
    }
    setLocationNote('');
    const lines = editor.value.split('\n');
    if (line > lines.length) return;
    const start = lines.slice(0, line - 1).reduce((offset, text) => offset + text.length + 1, 0);
    editor.focus();
    editor.setSelectionRange(start, start + lines[line - 1].length);
  }
  return <section className="queue-diagnostics" aria-label="执行诊断">
    {notice?.tone==='error'?<p className="queue-notice queue-notice--error" role="alert">{notice.text}</p>:null}
    {queue?.sync?.error?<p className="queue-notice queue-notice--error" role="alert">同步异常：{queueMessageText(queue.sync.error)} · 运行 {queue.runId} · 第 {queue.line} 行。停止请求不代表已停稳。</p>:null}
    {locationNote?<p role="status">{locationNote}</p>:null}
    {alert ? <div className="queue-notice queue-notice--error" role="alert">
      <strong>{labels[alert.confirmation] ?? alert.confirmation}</strong>
      {' · '}运行 {alert.runId} · 第 {alert.iteration} 轮 · 第 {alert.line} 行 · 电机 {alert.motor}
      {' · '}返回码 {hex(alert.code)}
      <button type="button" className="link-button" onClick={() => locate(alert.line)}>定位指令</button>
      <span>异常记录保留；成功轮询不表示问题已消失。</span>
    </div> : null}
    <details>
      <summary>查看发送与应答详情（{events.length} 条）</summary>
      <p>发送、驱动接受和机械到位是不同证据。未等待动作不额外查询；同轴同功能码的迟到应答可能无法归属。</p>
      {queue?.diagnosticsDropped > 0 ? <p>有界记录已淘汰 {queue.diagnosticsDropped} 条；首个异常另行保留。</p> : null}
      <div className="queue-diagnostics__table"><table>
        <thead><tr><th>运行／轮次</th><th>行／电机</th><th>功能码</th><th>发送 ms</th><th>应答 ms</th><th>确认程度／返回码</th></tr></thead>
        <tbody>{events.map(event => <tr key={event.sequence}>
          <td>{event.runId} / {event.iteration}</td>
          <td><button type="button" className="link-button" onClick={() => locate(event.line,event.runId)}>{event.line} / {event.motor}</button></td>
          <td>{hex(event.function)}</td><td>{event.sentAt}</td><td>{event.responseAt ?? '—'}</td>
          <td>{labels[event.confirmation] ?? event.confirmation} / {hex(event.code)}</td>
        </tr>)}</tbody>
      </table></div>
    </details>
  </section>;
}
