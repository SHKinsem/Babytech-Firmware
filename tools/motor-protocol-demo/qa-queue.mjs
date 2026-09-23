// Browser check for the 编排队列 tab against mocked board APIs — no hardware.
// Run after `npm run build:device` (the script reads the built device page).
//
//   node qa-queue.mjs
//
import {createRequire} from 'node:module';
import {readFile,mkdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
const page=await browser.newPage({viewport:{width:1513,height:1039}});
const html=await readFile(new URL('../../device-controller/data/index.html',import.meta.url),'utf8');
const errors=[],posts=[];
let seq=1,runId=0;
let startMode='ok',cancelMode='ok';
let queueStatus={state:'idle',runId:0,step:0,total:0,iteration:1,repeat:1,line:0,action:'',message:'',raw:false};
// Per-ID rotation distances: 1 = 40 mm/rev, 2 = 25 mm/rev, 3 = cleared (0).
const distances={1:40,2:25,3:0,5:30};
let distanceSaveFail=false,distanceWrongId=false,limitsOffline=false;
// Held requests, so the page's race handling can be observed deterministically.
let holdQueuePoll=false,holdDistanceId=0,holdDistanceSave=false;
const gate=()=>{const box={};box.promise=new Promise(resolve=>{box.resolve=resolve;});return box;};
const pollHeld=gate(),pollRelease=gate(),distancePollHeld=gate(),distancePollRelease=gate(),saveHeld=gate(),saveRelease=gate();
page.on('pageerror',e=>errors.push(e.message));
const starts=()=>posts.filter(entry=>entry.path==='/api/queue/start');
const cancels=()=>posts.filter(entry=>entry.path==='/api/queue/cancel');
/** Wait for text anywhere on the page; the first match keeps strict mode quiet. */
const waitText=pattern=>page.getByText(pattern).first().waitFor();
await page.route('http://queue.test/**',async route=>{
 const url=new URL(route.request().url());
 const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
 if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
 if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
 if(url.pathname==='/api/config-result')return json({sequence:0});
 if(route.request().method()==='POST'){
   const params=Object.fromEntries(new URLSearchParams(route.request().postData()));
   posts.push({path:url.pathname,params});
   if(url.pathname==='/api/queue/start'){
     if(startMode==='reject')return json({error:'program_line',line:2,message:'invalid program'},400);
     // A lost response: the board may or may not have started the program.
     if(startMode==='abort')return route.abort();
     runId+=1;
     queueStatus={state:'running',runId,step:1,total:7,iteration:1,repeat:Number(params.repeat)||1,line:3,action:'enable 1',message:'',raw:false};
     return json(queueStatus,202);
   }
   if(url.pathname==='/api/queue/cancel'){
     if(cancelMode==='abort')return route.abort();
     queueStatus={...queueStatus,state:'cancelled',runId:queueStatus.runId+1,message:'cancelled by user'};
     return json(queueStatus);
   }
   if(url.pathname==='/api/motor-distance'){
     if(distanceSaveFail)return json({error:'motor_distance_busy'},409);
     const id=Number(params.id);
     if(distanceWrongId)return json({id:id+1,rotationDistance:Number(params.rotationDistance)});
     const value=Number(params.rotationDistance);
     if(holdDistanceSave){holdDistanceSave=false;saveHeld.resolve();await saveRelease.promise;}
     distances[id]=value;
     return json({id,rotationDistance:value>0?value:0});
   }
   return json({message:'queued'});
 }
 if(url.pathname==='/api/limits')return limitsOffline?route.abort():json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
 if(url.pathname==='/api/queue'){
   if(holdQueuePoll){
     holdQueuePoll=false;
     // Snapshot taken when the read arrives, delivered after the start answer.
     const stale={...queueStatus};
     pollHeld.resolve();
     await pollRelease.promise;
     return json(stale);
   }
   return json(queueStatus);
 }
 if(url.pathname==='/api/motor-distance'){
   if(distanceSaveFail)return json({error:'motor_distance_busy'},409);
   const id=Number(url.searchParams.get('id'));
   const value=distances[id]>0?distances[id]:null;
   if(holdDistanceId===id){
     holdDistanceId=0;
     distancePollHeld.resolve();
     await distancePollRelease.promise;
     return json({id,rotationDistance:value});
   }
   return json({id,rotationDistance:value});
 }
 if(url.pathname==='/api/status')return json({id:Number(url.searchParams.get('id')),canReady:true,online:true,enabled:true,state:'idle',positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:true,fault:'none',homeOutcome:'none'});
 if(url.pathname==='/api/trace')return json({uptimeMs:10000,sequence:seq,frames:[]});
 return json({});
});
try {
 await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
 await page.goto('http://queue.test/');
 await waitText('设备在线');
 const tab=name=>page.getByRole('tab',{name,exact:true}).click();
 const startButton=page.getByRole('button',{name:'开始执行',exact:true});
 const cancelButton=page.getByRole('button',{name:'取消队列',exact:true});

 // A stored draft must never submit anything on its own.
 await tab('编排队列');
 const programField=page.locator('#queue-program');
 await programField.waitFor();
 assert.equal(starts().length,0,'opening the tab must not start anything');
 assert.equal(await startButton.isDisabled(),true,'an empty program cannot start');
 await waitText(/没有任何动作/);

 // Sample program, preview with source lines, units and defaults.
 await page.getByRole('button',{name:'载入示例',exact:true}).click();
 await waitText(/相对运动 \+90 deg/);
 const preview=await page.locator('.queue-col--preview').innerText();
 assert.match(preview,/电机 1：相对运动 \+90 deg/);
 assert.match(preview,/电机 2：发送回零触发 9A · 模式 0 单圈就近/);
 assert.match(preview,/电机 3：限速力矩 \+800 mA · 1500 ms/);
 assert.doesNotMatch(preview,/\d\s*Nm/,'torque is never given as an Nm value');
 assert.equal(await page.locator('.queue-preview__row').count(),8);
 assert.equal(await page.locator('.queue-preview__line').first().innerText(),'3','preview rows carry source line numbers');
 assert.equal(await page.getByText('动作 8/64',{exact:false}).count()>0,true);
 assert.equal(await startButton.isEnabled(),true);
 assert.equal(starts().length,0);

 // Start posts exactly once, and only the board's own status is displayed.
 await startButton.click();
 await waitText(/板端已接受队列（HTTP 202）/);
 assert.equal(starts().length,1,'start must be submitted exactly once');
 assert.match(starts()[0].params.program,/enable 1/);
 assert.equal(starts()[0].params.repeat,'1');
 assert.equal(await startButton.isDisabled(),true,'a running queue cannot be started again');

 // Live progress comes from the poll, never from an optimistic local guess.
 queueStatus={...queueStatus,step:3,line:7,action:'move 1 90',total:7};
 await waitText('3/7');
 await waitText('第 7 行');

 // Queue busy locks the manual tab; stop stays available.
 await tab('常规试动');
 const sendMove=page.getByRole('button',{name:'发送运动指令',exact:true});
 const manualGate=page.locator('.manual__form .actions__gate');
 await manualGate.waitFor();
 assert.match(await manualGate.innerText(),/队列正在运行：常规试动已锁定/);
 assert.equal(await sendMove.isDisabled(),true,'manual motion is locked while the queue runs');
 assert.equal(await page.getByRole('button',{name:'立即停止',exact:true}).isEnabled(),true,'stop stays available');
 await tab('编排队列');

 // Cancel is always available and releases the lock once the board confirms.
 await cancelButton.click();
 await waitText(/取消已由板端确认/);
 assert.equal(cancels().length,1);
 await tab('常规试动');
 await manualGate.waitFor({state:'detached'});
 assert.equal(await sendMove.isEnabled(),true,'manual motion returns after the board confirms the cancel');
 await tab('编排队列');

 // Per-ID rotation distances stay independent, and a refused save changes nothing.
 const distanceField=page.getByLabel('旋转距离 mm/rev',{exact:true});
 const addressField=page.getByLabel('旋转距离电机地址',{exact:true});
 await waitText('电机 1：已确认 40 mm/rev');
 await addressField.fill('2');
 await waitText('电机 2：已确认 25 mm/rev');
 assert.equal(await distanceField.inputValue(),'25');
 await addressField.fill('1');
 await waitText('电机 1：已确认 40 mm/rev');
 assert.equal(await distanceField.inputValue(),'40','another ID must not overwrite this profile');
 await addressField.fill('3');
 await waitText('电机 3：已确认为 0（未配置）');
 assert.equal(await distanceField.inputValue(),'','a cleared distance shows no value');

 distanceSaveFail=true;
 await addressField.fill('1');
 await distanceField.fill('41');
 await page.getByRole('button',{name:'保存到板上',exact:true}).click();
 await waitText(/保存失败（HTTP 409）/);
 assert.equal(distances[1],40,'a refused save must not change the board value');
 await waitText('电机 1：已确认 40 mm/rev');

 distanceSaveFail=false;
 distanceWrongId=true;
 await page.getByRole('button',{name:'保存到板上',exact:true}).click();
 await waitText(/电机地址.*与请求.*不一致/);
 assert.equal(distances[1],40,'a mismatched response is not adopted');
 distanceWrongId=false;

 // A save in flight freezes both fields, and a status read that was already on
 // the wire when the save answered must not overwrite what the board confirmed.
 holdDistanceId=5;
 await addressField.fill('5');
 await distancePollHeld.promise;                 // the read for 5 is held now
 await distanceField.fill('55');
 holdDistanceSave=true;
 await page.getByRole('button',{name:'保存到板上',exact:true}).click();
 await saveHeld.promise;                         // the write is on the wire now
 assert.equal(await addressField.isDisabled(),true,'the address field freezes while saving');
 assert.equal(await distanceField.isDisabled(),true,'the value field freezes while saving');
 saveRelease.resolve();
 await waitText('已保存电机 5 的旋转距离：55 mm/rev');
 assert.equal(await distanceField.inputValue(),'55');
 distancePollRelease.resolve();                  // the stale read answers 30
 await page.waitForTimeout(400);
 assert.equal(await distanceField.inputValue(),'55','a stale read must not overwrite the posted save');
 await waitText('电机 5：已确认 55 mm/rev');
 assert.equal(await distanceField.isDisabled(),false,'the fields unlock once the save is answered');
 assert.equal(distances[5],55);

 // mm uses the confirmed distance; an unconfigured ID blocks the start.
 await programField.fill('enable 1\nenable 2\nmove 1 90 mm\n');
 await waitText(/旋转距离 40 mm\/rev → 810°/);
 assert.equal(await startButton.isEnabled(),true);
 await programField.fill('enable 3\nmove 3 90 mm\n');
 await waitText(/旋转距离为 0（未配置）/);
 assert.equal(await startButton.isDisabled(),true,'mm without a saved distance cannot start');

 // A rejected program reports the board's line and never retries itself.
 await programField.fill('enable 1\nmove 1 90\n');
 await waitText(/电机 1：相对运动 \+90 deg/);
 startMode='reject';
 const before=starts().length;
 await startButton.click();
 await waitText(/板端拒绝（HTTP 400） · 源程序第 2 行/);
 assert.equal(starts().length,before+1,'a rejected start is not retried');
 assert.equal(await startButton.isEnabled(),true,'a rejected program can be corrected and resubmitted');
 await page.waitForTimeout(3000);
 assert.equal(starts().length,before+1,'no automatic retry after a rejection');

 // A lost response stays "unknown" and keeps the lock until the board answers.
 // The board keeps reporting a quiet queue, which is exactly the ambiguous
 // case: an unconfirmed submission must not be read as "nothing is running".
 queueStatus={...queueStatus,state:'idle',step:0,total:0,line:0,action:'',message:''};
 startMode='abort';
 await startButton.click();
 await waitText(/提交结果未知/);
 assert.equal(await startButton.isDisabled(),true,'an unknown submission keeps the page locked');
 await tab('常规试动');
 await manualGate.waitFor();
 assert.match(await manualGate.innerText(),/队列提交结果未知/);
 assert.equal(await sendMove.isDisabled(),true);
 await tab('编排队列');
 startMode='ok';
 await cancelButton.click();
 await waitText(/取消已由板端确认/);
 await tab('常规试动');
 assert.equal(await sendMove.isEnabled(),true,'cancelling clears the unknown-execution lock');
 await tab('编排队列');

 // A status read that was already on the wire when the start was answered must
 // not be able to report the pre-start state afterwards (and must not unlock).
 queueStatus={...queueStatus,state:'idle',step:0,total:0,line:0,action:'',message:''};
 holdQueuePoll=true;
 await page.getByRole('button',{name:'刷新状态',exact:true}).click();
 await pollHeld.promise;                          // the read is held now
 await startButton.click();
 await waitText(/板端已接受队列（HTTP 202）/);
 pollRelease.resolve();                           // the stale idle answer lands
 // Longer than the round trip, shorter than the next scheduled poll, so only
 // the stale answer could have changed anything in this window.
 await page.waitForTimeout(500);
 assert.match(await page.locator('.queue-col--side').innerText(),/运行中/,'a stale pre-start read must not overwrite the running state');
 assert.equal(await startButton.isDisabled(),true,'the running queue is still locked against a second start');
 await tab('常规试动');
 await manualGate.waitFor();
 assert.equal(await sendMove.isDisabled(),true,'a stale pre-start read must not unlock the page');
 await tab('编排队列');
 await cancelButton.click();
 await waitText(/取消已由板端确认/);

 // Unknown board limits are never turned into assumed policy: the page says so
 // and leaves the whole program to the board's own validation.
 limitsOffline=true;
 await page.waitForTimeout(1500);
 assert.equal(await startButton.isEnabled(),true,'the board validates queue policy, not the page');
 limitsOffline=false;

 // Desktop layouts: no horizontal overflow, the send controls stay reachable.
 for(const size of [{width:1280,height:800},{width:1513,height:1039}]) {
   await page.setViewportSize(size);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   await startButton.scrollIntoViewIfNeeded();
   const box=await startButton.boundingBox();
   assert.ok(box&&box.x>=0&&box.x+box.width<=size.width&&box.y>=0&&box.y+box.height<=size.height,'start button reaches the viewport');
   const trace=await page.locator('.device-trace').boundingBox();
   assert.ok(trace&&trace.y+trace.height<=size.height+1,'the bottom bus trace stays visible');
   await page.screenshot({path:fileURLToPath(new URL(`./qa/queue-${size.width}.png`,import.meta.url)),fullPage:true});
 }

 // The draft is local only and survives a reload without executing anything.
 const draft=await page.evaluate(()=>localStorage.getItem('motor-protocol-demo.queue.draft.v1'));
 assert.match(draft,/move 1 90/);
 const started=starts().length;
 await page.reload();
 await waitText('设备在线');
 await tab('编排队列');
 await programField.waitFor();
 assert.match(await programField.inputValue(),/move 1 90/,'the local draft is restored');
 assert.equal(starts().length,started,'reloading never starts the queue');

 assert.deepEqual(errors,[]);
 console.log('PASS: queue DSL preview, single start POST, board-reported progress, cancel recovery, busy locking, per-ID rotation distances, refused saves, unknown delivery, no auto retry and desktop layouts (mock APIs; no hardware).');
} finally {await browser.close();}
