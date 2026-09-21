import { useEffect, useState } from 'react';
import { request, errorLabels } from '../device-api.js';

export function WifiPanel({ notify }) {
  const [wifi, setWifi] = useState(null), [scan,setScan] = useState(null);
  const [ssid,setSsid] = useState(''), [password,setPassword] = useState('');
  const [pending,setPending] = useState(false), [offline,setOffline] = useState(false);
  useEffect(() => {
    let disposed = false, timer;
    const controller = new AbortController();
    async function poll() {
      try {
        const status = await request('/api/wifi', undefined, controller.signal);
        if (disposed) return;
        setWifi(status); setOffline(false);
        const result = await request('/api/wifi/scan', undefined, controller.signal);
        if (!disposed) setScan(result);
      } catch { if (!disposed) setOffline(true); }
      if (!disposed) timer = setTimeout(poll, 1500);
    }
    poll(); return () => { disposed = true; controller.abort(); clearTimeout(timer); };
  }, []);
  async function act(path, data) {
    if (pending) return;
    setPending(true);
    try {
      const result = await request(path,data);
      if (path.endsWith('/connect')) { setPassword(''); setWifi(w => ({...w,state:'connecting',busy:true})); }
      if (path.endsWith('/scan')) setScan({state:result.state,networks:[]});
      notify('请求已接收，等待设备更新状态');
    } catch (e) { notify(errorLabels[e.message] || e.message); }
    finally { setPending(false); }
  }
  const busy = pending || wifi?.busy || offline;
  return <section className="wifi-page" aria-label="Wi-Fi 设置">
    <div className="wifi-card"><h2>网络连接</h2><p>设备热点始终保留。扫描和切换网络前，请停止电机并关闭使能。</p>
      <dl className="metrics"><div className="metrics__row"><dt>局域网状态</dt><dd>{offline ? '设备连接中断' : ({connected:'已连接',connecting:'连接中',failed:'连接失败',idle:'未连接'}[wifi?.state] || '读取中')}</dd></div>
      <div className="metrics__row"><dt>已保存网络</dt><dd>{wifi?.saved ? wifi.ssid : '—'}</dd></div>
      <div className="metrics__row"><dt>局域网地址</dt><dd>{wifi?.ip ? <a href={`http://${wifi.ip}/`}>{wifi.ip}</a> : '—'}</dd></div>
      <div className="metrics__row"><dt>设备热点</dt><dd>{wifi?.apSsid || '—'}</dd></div>
      <div className="metrics__row"><dt>热点地址</dt><dd>{wifi?.apIp || '—'}</dd></div></dl>
      {wifi?.error && <p role="alert">{wifi.error}</p>}
      <button className="button button--outline" disabled={busy || !wifi?.saved} onClick={() => act('/api/wifi/forget',{})}>忘记已保存网络</button>
    </div>
    <form className="wifi-card" onSubmit={e => {e.preventDefault();act('/api/wifi/connect',{ssid,password});}}>
      <h2>加入 Wi-Fi</h2><div className="wifi-scan"><button type="button" className="button button--outline" disabled={busy || scan?.state === 'scanning'} onClick={() => act('/api/wifi/scan',{})}>{scan?.state === 'scanning' ? '扫描中…' : '扫描网络'}</button><span>{scan?.state === 'failed' ? '扫描失败，可手动输入' : '也可以手动填写网络名称'}</span></div>
      <div className="wifi-networks">{scan?.networks?.map(n => <button key={n.ssid} type="button" onClick={() => {setSsid(n.ssid);setPassword('');}}><span>{n.ssid}</span><span>{n.secure ? '加密' : '开放'} · {n.rssi} dBm</span></button>)}</div>
      <label htmlFor="wifi-ssid">网络名称</label><input id="wifi-ssid" className="input" value={ssid} onChange={e => setSsid(e.target.value)} required autoComplete="off" />
      <label htmlFor="wifi-password">密码（开放网络留空）</label><input id="wifi-password" className="input" type="password" value={password} onChange={e => setPassword(e.target.value)} autoComplete="new-password" />
      <button className="button button--primary" disabled={busy || !ssid.trim()}>保存并连接</button>
      <p>密码仅发送到设备并保存在其 NVS 中。连接时若页面断开，请重新连接设备热点，或通过局域网地址访问。</p>
    </form>
  </section>;
}
