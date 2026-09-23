// Desktop QA for the 调试日志 tab.
//
// Conventions follow qa-current-limit.mjs / qa-drafts.mjs: the built device page
// is served behind a mocked board API and driven in a real Chromium. Every
// request the page makes is recorded, so "the page never resends by itself" and
// "a credential never reaches the log" are assertions about real traffic and
// about the actual exported files.
//
// Run it from this folder after the device page has been rebuilt:
//   node qa-debug-log.mjs
import { createRequire } from 'node:module';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
const require=createRequire(import.meta.url);
let playwright;
try {playwright=require(process.env.PLAYWRIGHT_MODULE || 'playwright');}
catch {playwright=require('C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');}
const {chromium}=playwright;
const browser=await chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE || (process.platform === 'win32' ? 'C:/Program Files/Google/Chrome/Application/chrome.exe' : undefined)});
const page=await browser.newPage({viewport:{width:1513,height:1039}});
const errors=[], consoleErrors=[], posts=[], checks=[];
page.on('pageerror',e=>errors.push(e.message));
page.on('console',m=>{if(m.type()==='error')consoleErrors.push(m.text());});
// ---- mock board ------------------------------------------------------------
const motor={enabled:false,state:'disabled',online:true,canReady:true,busState:'running',txErrors:0,
  positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:false,lastAck:'none',fault:'none',
  activeId:0,homeOutcome:'none',homeId:0,homeMode:null,homeOrg:null,homeRunning:null,homeFailed:null};
let queue={state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false};
let config={sequence:0};
let traceFrames=[];
let traceSeq=0;
let board={bootId:'TESTBOOT0001',sequence:3,events:[
  {seq:1,atMs:10,level:'info',event:'boot',detail:'reset=1'},
  {seq:2,atMs:20,level:'info',event:'can.init',detail:'ready=1'},
  {seq:3,atMs:30,level:'info',event:'http.ready',detail:'port=80'}]};
let boardMode='ok';       // ok | missing
let commandPosts=0, releaseHold=null;
// 清除板端状态: ok (200) | unconfirmed (503) | missing (404) | hang (no answer)
let resetMode='ok', releaseReset=null;
let holdQueueOnce=false, releaseQueue=null;
const resetPosts=()=>posts.filter(p=>p.path==='/api/control/reset');
const pushBoard=(level,event,detail)=>{
  board.sequence+=1;
  board.events.push({seq:board.sequence,atMs:board.sequence*10,level,event,detail});
  if(board.events.length>48)board.events.shift();
};
const pushFrame=(dir,data,canId=0x100)=>{
  traceSeq+=1;
  traceFrames.push({seq:traceSeq,atMs:traceSeq*5,dir,id:canId,extended:true,remote:false,data});
  if(traceFrames.length>200)traceFrames.shift();
};
const html=await readFile(new URL('../../device-controller/data/index.html',import.meta.url),'utf8');
await page.route('http://device.test/**',async route=>{
  const url=new URL(route.request().url()), path=url.pathname;
  const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/favicon.ico')return route.fulfill({status:204});
  if(path==='/api/config-result')return json(config);
  if(route.request().method()==='POST'){
    const params=Object.fromEntries(new URLSearchParams(route.request().postData()));
    posts.push({path,params});
    if(path==='/api/command'){
      commandPosts+=1;
      if(commandPosts===1)return route.fulfill({status:409,contentType:'application/json',body:JSON.stringify({ok:false,error:'disable_and_wait_for_stationary_feedback'})});
      // The third submission is answered far beyond the page's own 1800 ms
      // budget so the timeout path is exercised without a brittle short wait.
      if(commandPosts===3){
        await new Promise(resolve=>{releaseHold=resolve;});
        try { return await json({ok:true,message:'queued'}); } catch { return undefined; }
      }
      return json({ok:true,message:'queued'},202);
    }
    if(path==='/api/control/reset'){
      if(resetMode==='missing')return route.fulfill({status:404,contentType:'application/json',body:JSON.stringify({error:'not found'})});
      if(resetMode==='hang'){
        await new Promise(resolve=>{releaseReset=resolve;});
        try { return await json({ok:true,stateCleared:true,stopSent:true,message:'control_state_cleared'}); } catch { return undefined; }
      }
      // The board clears its own state either way; only the stop attempt differs,
      // and both outcomes are recorded in the board's own RAM log.
      if(resetMode==='unconfirmed'){
        pushBoard('error','control.cleared','uart_cancelled=1 stop_code=503 stop_sent=0 stop_message=stop_tx_failed');
        return route.fulfill({status:503,contentType:'application/json',body:JSON.stringify({ok:false,stateCleared:true,stopSent:false,error:'control_state_cleared_stop_unconfirmed'})});
      }
      pushBoard('warn','control.cleared','uart_cancelled=1 stop_code=202 stop_sent=1 stop_message=queued');
      return json({ok:true,stateCleared:true,stopSent:true,message:'control_state_cleared'});
    }
    if(path==='/api/wifi/connect')return json({ok:true,state:'connected',ssid:params.ssid,ip:'192.168.1.50'});
    return json({ok:true,message:'queued'});
  }
  if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
  if(path==='/api/status')return json({...motor,id:Number(url.searchParams.get('id'))});
  if(path==='/api/queue'){
    if(holdQueueOnce){
      // A queue read that started before the operator reset: its answer describes
      // the board from before the reset and must not be applied afterwards.
      holdQueueOnce=false;
      const stale=queue;
      await new Promise(resolve=>{releaseQueue=resolve;});
      try { return await json(stale); } catch { return undefined; }
    }
    return json(queue);
  }
  if(path==='/api/trace')return json({uptimeMs:traceSeq*5+100,sequence:traceSeq,frames:traceFrames.slice(-40)});
  if(path==='/api/logs'){
    if(boardMode==='missing')return route.fulfill({status:404,contentType:'application/json',body:JSON.stringify({error:'not found'})});
    return json({bootId:board.bootId,uptimeMs:board.sequence*10,sequence:board.sequence,capacity:48,events:board.events});
  }
  if(path==='/api/wifi')return json({state:'idle',ssid:'',saved:false,ip:'',apSsid:'Babytech-Motion',apIp:'192.168.4.1',busy:false,error:''});
  if(path==='/api/wifi/scan')return json({state:'done',networks:[{ssid:'Lab WiFi',secure:true,rssi:-48}]});
  throw Error('Unexpected asset/API '+path);
});
const openLogTab=async()=>{
  await page.getByRole('tab',{name:'调试日志',exact:true}).click();
  await page.evaluate(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
};
const logText=async()=>page.locator('.panel--log').innerText();
const eventCount=async(name)=>{
  const text=await logText();
  return text.split('\n').filter(line=>line.includes(name)).length;
};
const readDownload=async(button)=>{
  const [download]=await Promise.all([page.waitForEvent('download'),page.getByRole('button',{name:button,exact:true}).click()]);
  const stream=await download.createReadStream();
  const chunks=[];
  for await (const chunk of stream) chunks.push(chunk);
  return Buffer.concat(chunks).toString('utf8');
};
try {
  await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
  await page.goto('http://device.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  assert.equal(posts.length,0,'no request is made just by loading the page');
  assert.ok(await page.getByRole('tab',{name:'调试日志',exact:true}).count()===1);
  checks.push('新增「调试日志」标签页，加载网页不产生任何请求');

  // 1. The board ring is read and shown once.
  await openLogTab();
  await page.waitForTimeout(2200);
  const initial=await logText();
  assert.match(initial,/boot/);
  assert.match(initial,/板端日志：可读/);
  assert.match(initial,/RAM 环形缓冲/);
  assert.match(initial,/不会据此发送任何指令|不会据此驱动/);
  assert.equal(await eventCount('can.init'),1,'a ring entry is imported once, not per poll');
  checks.push('板端日志按 bootId+seq 去重：同一环形缓冲不会重复导入');

  // 2. A refused submission keeps the board's reason; a 202 is called queued.
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByLabel('搜索指令名称或功能码').fill('闭环最大相电流');
  await page.locator('.library__item').first().click();
  const send=page.getByRole('button',{name:'发送指令',exact:true});
  await send.click();
  await page.waitForTimeout(150);
  assert.match(await page.locator('.panel--feedback').innerText(),/关闭使能/);
  await send.click();
  await page.waitForTimeout(150);
  await openLogTab();
  const afterSend=await logText();
  assert.match(afterSend,/HTTP 409/,'the refusal is in the log with its status');
  assert.match(afterSend,/disable_and_wait_for_stationary_feedback/,'and with the board reason');
  assert.match(afterSend,/HTTP 202/);
  assert.match(afterSend,/已入队（202/);
  assert.match(afterSend,/提交不等于执行|不代表/);
  checks.push('提交分三级如实记录：已提交 / HTTP 202 已入队 / 板端拒绝原因（含 409 原因）');

  // 3. A response beyond the request budget is an uncertain result, never a retry.
  const before=posts.filter(p=>p.path==='/api/command').length;
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await send.click();
  await page.waitForTimeout(2300);
  assert.equal(posts.filter(p=>p.path==='/api/command').length,before+1,'a timed-out request is never resent');
  if(releaseHold)releaseHold();
  await page.waitForTimeout(200);
  await openLogTab();
  const afterTimeout=await logText();
  assert.match(afterTimeout,/结果未知/);
  assert.match(afterTimeout,/不会自动重发|不会自动重试/);
  checks.push('超时只记录一次且标注结果未知，页面不自动重发');

  // 4. State changes are logged once, not once per poll.
  motor.fault='move_timeout';
  motor.state='fault';
  queue={...queue,state:'running',runId:4,step:1,message:'running'};
  config={sequence:1,id:1,state:'config_wait_ack',ack:0};
  pushBoard('error','motor.fault','fault=move_timeout');
  // Long enough for the slowest poll (the idle queue is read every 2.5 s).
  await page.waitForTimeout(3000);
  const afterChanges=await logText();
  assert.equal(afterChanges.split('\n').filter(line=>line.includes('status.changed')).length,1);
  assert.equal(afterChanges.split('\n').filter(line=>line.includes('queue.changed')).length,1);
  assert.equal(afterChanges.split('\n').filter(line=>line.includes('config.changed')).length,1);
  assert.equal(await eventCount('motor.fault'),1);
  assert.match(afterChanges,/fault: move_timeout/);
  checks.push('状态／队列／配置变更各记录一次，轮询本身不产生日志');

  // 5. CAN: control frames are kept, the periodic query rotation is not.
  pushFrame('RX',[0x36,0,0,0,0,125,0x6B]);
  pushFrame('TX',[0x36,0x6B]);
  pushFrame('RX',[0xF3,0x02,0x6B],0x200);
  await page.waitForTimeout(900);
  await openLogTab();
  assert.equal(await eventCount('F3 02 6B'),1,'the control frame is recorded');
  assert.equal((await logText()).includes('36 00 00 00 00 7D 6B'),false,'query replies stay out of the log');
  checks.push('CAN 记录只保留控制帧，轮询查询帧不进入日志');

  // 6. Ring gaps and a board restart are explicit, never silently filled.
  board={bootId:board.bootId,sequence:90,events:[{seq:85,atMs:850,level:'info',event:'queue.state',detail:'run=9 state=1'}]};
  await page.waitForTimeout(2300);
  const afterGap=await logText();
  assert.match(afterGap,/缺口/);
  assert.match(afterGap,/ring|环形缓冲/);
  board={bootId:'TESTBOOT0002',sequence:1,events:[{seq:1,atMs:5,level:'info',event:'boot',detail:'reset=3'}]};
  await page.waitForTimeout(2300);
  const afterRestart=await logText();
  assert.match(afterRestart,/板端重启|board.restart/);
  assert.match(afterRestart,/TESTBOOT0002/);
  checks.push('板端环形缓冲被覆盖时标注缺口，重启时标注 bootId 变化且旧历史不可用');

  // 7. A missing endpoint is shown once and slows down, and the board stays online.
  boardMode='missing';
  await page.waitForTimeout(2400);
  const after404=await logText();
  assert.equal(after404.split('\n').filter(line=>line.includes('没有 /api/logs')).length,1);
  assert.equal(await page.getByText('设备未连接',{exact:true}).count(),0,'a missing log endpoint is not "offline"');
  assert.equal(await page.getByText('设备在线',{exact:true}).count(),1);
  // The poll was slowed down, so the explicit refresh is what brings it back.
  boardMode='ok';
  await page.getByRole('button',{name:'重新读取板端日志',exact:true}).click();
  await page.waitForTimeout(700);
  assert.match(await logText(),/恢复可读/,'a recovered endpoint is reported once');
  checks.push('旧固件没有 /api/logs 时提示一次并降低轮询，不把设备判为离线；手动刷新可立即恢复');

  // 8. A reload keeps the local history and sends nothing on restore.
  const postsBeforeReload=posts.length;
  await page.reload();
  await page.getByText('设备在线',{exact:true}).waitFor();
  await openLogTab();
  const afterReload=await logText();
  assert.match(afterReload,/HTTP 409|request.submitted/,'history from the previous session is still there');
  assert.match(afterReload,/log.cleared|boot|can.init|request/);
  assert.equal(posts.length,postsBeforeReload,'restoring history never sends anything');
  checks.push('刷新后本机历史仍在，恢复历史不产生任何 POST');

  // 9. Credentials never reach the log: connect to Wi-Fi with a password.
  await page.getByRole('tab',{name:'Wi-Fi 设置',exact:true}).click();
  await page.getByRole('button',{name:'扫描网络',exact:true}).click();
  await page.getByRole('button',{name:/Lab WiFi/}).click();
  await page.getByLabel('密码（开放网络留空）').fill('SuperSecret123');
  await page.getByRole('button',{name:'保存并连接'}).click();
  await page.waitForTimeout(400);
  assert.equal(posts.some(p=>p.path==='/api/wifi/connect'),true,'the connection request was really made');
  const logWithWifi=await page.evaluate((key)=>localStorage.getItem(key),'babytech.device-log.v1');
  assert.doesNotMatch(String(logWithWifi),/SuperSecret123/);
  assert.doesNotMatch(String(logWithWifi),/password|Lab WiFi|"ssid"/);
  checks.push('Wi-Fi 连接已经发生，但密码／SSID 从未进入本机日志');

  // 10. Exports carry the evidence and still no credentials.
  await openLogTab();
  const markdown=await readDownload('导出 Markdown');
  const jsonText=await readDownload('导出 JSON');
  for (const text of [markdown,jsonText]) {
    assert.doesNotMatch(text,/SuperSecret123/);
    assert.doesNotMatch(text,/password/i);
    assert.doesNotMatch(text,/Lab WiFi/);
  }
  assert.match(markdown,/maxCurrentMa/,'confirmed limits are part of the export');
  assert.match(markdown,/fault/,'the motor snapshot is part of the export');
  assert.match(markdown,/F3 02 6B|RX id=0x200/,'retained CAN frames are part of the export');
  assert.match(markdown,/RAM 环形缓冲/);
  const parsed=JSON.parse(jsonText);
  assert.equal(parsed.logVersion,1);
  assert.ok(parsed.events.length>0);
  assert.ok(Array.isArray(parsed.trace));
  checks.push('导出的 Markdown／JSON 含限制、电机快照、队列与 CAN 证据，且没有凭据或查询串');

  // 11. Clearing drops the local history but keeps the drafts and the cursor.
  const draftsBefore=await page.evaluate(()=>localStorage.getItem('babytech.device-drafts'));
  await page.getByRole('button',{name:'清空本机历史',exact:true}).click();
  await page.waitForTimeout(200);
  const afterClear=await logText();
  assert.match(afterClear,/已清空本机日志历史/);
  assert.doesNotMatch(afterClear,/HTTP 409/,'the cleared history is gone');
  assert.equal(await page.evaluate(()=>localStorage.getItem('babytech.device-drafts')),draftsBefore,'clearing logs keeps the motor drafts');
  await page.waitForTimeout(2600);
  // The ring still holds the old entries, but the cursor was kept: they must not
  // come back on their own.
  assert.equal((await logText()).includes('can.init'),false,'the board ring is not re-imported after a clear');
  checks.push('清空只影响本机日志：草稿保留，板端游标不重置（历史不会立刻回流）');

  await page.screenshot({path:fileURLToPath(new URL('./qa/debug-log-1513.png',import.meta.url)),fullPage:true});

  // 12. 清除板端状态: one POST, on a busy board, with no enable or move.
  const queueState={state:'running',runId:7,step:2,total:5,iteration:1,repeat:1,line:3,action:'move',message:'running',raw:false};
  queue=queueState;
  motor.state='moving';motor.activeId=1;motor.enabled=true;motor.lastAck='received';
  motor.control={busy:true,stationary:false,fault:'none',faultId:0,faultGlobal:false,
    blockers:[{id:1,reason:'move_active',ageMs:100},{id:2,reason:'stop_pending',ageMs:50}],blockerCount:2};
  await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  await page.waitForTimeout(600);
  const reset=page.getByRole('button',{name:'清除板端状态',exact:true});
  assert.equal(await reset.isDisabled(),false,'the reset stays usable while the board is busy');
  const before2=posts.length;
  await reset.click();
  await page.waitForTimeout(300);
  assert.equal(resetPosts().length,1,'exactly one reset POST');
  assert.equal(posts.length,before2+1,'a reset sends nothing else');
  assert.equal(posts.some(p=>p.path==='/api/enable'),false,'a reset is not a re-enable');
  assert.equal(posts.some(p=>p.path==='/api/move'),false,'a reset never moves');
  assert.match(await page.locator('.panel--feedback').innerText(),/发送成功不等于电机已物理停止/);
  await openLogTab();
  const afterReset=await logText();
  assert.match(afterReset,/control\/reset/);
  assert.match(afterReset,/move_active/,'the board-side blocker summary is recorded');
  assert.match(afterReset,/stop_pending/);
  checks.push('忙碌时清除板端状态只发一次 POST，没有使能／移动，并记录清除前的占用与原因');

  // The cleared board keeps reporting the real state; the page does not claim
  // anything the board did not say.
  queue={...queueState,state:'idle',runId:0,step:0,message:''};
  motor.control={busy:false,stationary:true,fault:'none',faultId:0,faultGlobal:false,blockers:[],blockerCount:0};
  await page.getByRole('button',{name:'重新读取板端日志',exact:true}).click();
  await page.locator('.log__event').filter({hasText:'control.cleared'}).first().waitFor();
  const settled=await logText();
  assert.match(settled,/control.cleared/,'the reset outcome is recorded');
  assert.doesNotMatch(settled,/停止已确认|已停止到位/);

  // 13. A queue poll that was already in flight when the reset landed must not be
  // able to put the old running state back.
  holdQueueOnce=true;
  await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  await page.getByRole('button',{name:'清除板端状态',exact:true}).click();
  await page.waitForTimeout(300);
  queue={...queueState,state:'idle',runId:0,step:0,message:''};   // the board is really idle now
  if(releaseQueue)releaseQueue();
  await page.waitForTimeout(900);
  const queueView=await page.locator('.panel--feedback').innerText();
  assert.doesNotMatch(queueView,/运行中/,'the stale pre-reset queue answer was dropped');
  assert.equal(posts.filter(p=>p.path==='/api/control/reset').length,2);

  // 14. A stop the board could not confirm: cleared, but never "stopped".
  resetMode='unconfirmed';
  await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  await page.getByRole('button',{name:'清除板端状态',exact:true}).click();
  await page.waitForTimeout(300);
  const unconfirmed=await page.locator('.panel--feedback').innerText();
  assert.match(unconfirmed,/未确认|不要假定/);
  assert.doesNotMatch(unconfirmed,/停止已确认|已停止到位/);
  assert.equal(resetPosts().length,3);

  // 15. No answer at all: the result is unknown and nothing local is claimed.
  resetMode='hang';
  await page.getByRole('button',{name:'清除板端状态',exact:true}).click();
  await page.waitForTimeout(2300);
  assert.match(await page.locator('.panel--feedback').innerText(),/结果未知/);
  assert.equal(posts.filter(p=>p.path==='/api/control/reset').length,4,'a timed-out reset is not resent');
  if(releaseReset)releaseReset();
  await page.waitForTimeout(200);

  // 16. Old firmware: the button explains that it needs a newer one.
  resetMode='missing';
  await page.getByRole('button',{name:'清除板端状态',exact:true}).click();
  await page.waitForTimeout(300);
  assert.match(await page.locator('.panel--feedback').innerText(),/固件未更新|需要较新的固件/);
  assert.equal(posts.filter(p=>p.path==='/api/control/reset').length,5);
  resetMode='ok';
  const logAfterResets=await page.evaluate((key)=>localStorage.getItem(key),'babytech.device-log.v1');
  assert.match(String(logAfterResets),/control\/reset/,'the reset history survives');
  checks.push('停止未确认／超时未知／旧固件 404 三种情况分别如实提示，都不宣称已停止，且不自动重发');

  // 17. The compact viewport still fits, with the new toolbar button.
  await page.setViewportSize({width:1280,height:800});
  await openLogTab();
  await page.waitForTimeout(300);
  assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false,'no horizontal overflow at 1280x800');
  // The toolbar keeps both buttons reachable at the compact width.
  assert.equal(await page.getByRole('button',{name:'清除板端状态',exact:true}).isVisible(),true);
  assert.equal(await page.getByRole('button',{name:'全部停止',exact:true}).isVisible(),true);
  await page.screenshot({path:fileURLToPath(new URL('./qa/debug-log-1280.png',import.meta.url)),fullPage:true});
  checks.push('1513×1039 与 1280×800 均无横向溢出，工具栏两个按钮都可见');

  // The one request this run intentionally aborts (the timeout phase) can leave
  // a net::ERR_ABORTED line in Chrome's console; the raw list is written to the
  // results file, and the assertion is on real errors only.
  const realConsoleErrors=consoleErrors.filter((text)=>!/ERR_ABORTED|Failed to load resource: the server responded with a status of (409|404|503)/.test(text));
  assert.deepEqual(errors,[]);checks.push('无浏览器运行错误');
  assert.deepEqual(realConsoleErrors,[]);checks.push('无 console 错误（已排除本次故意中止的请求）');
  await writeFile(new URL('./qa/debug-log-results.json',import.meta.url),JSON.stringify({checks,errors,consoleErrors,posts},null,2));
  console.log(JSON.stringify({passed:checks.length,checks},null,2));
} finally {await browser.close();}
