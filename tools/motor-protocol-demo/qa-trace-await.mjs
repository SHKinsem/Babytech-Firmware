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
let seq=30,runId=0; let gets=0; let autoQueriesEnabled=true;
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
 const url=new URL(route.request().url()); if(route.request().method()==='GET' && url.pathname.startsWith('/api/')) gets++;
 const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
 if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
 if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
 if(url.pathname==='/api/config-result')return json({sequence:0});
 if(route.request().method()==='POST'){
   if (url.pathname==='/api/polling') {autoQueriesEnabled=new URLSearchParams(route.request().postData()).get('enabled')==='1';return json({autoQueriesEnabled});}
   const params=Object.fromEntries(new URLSearchParams(route.request().postData()));
   posts.push({path:url.pathname,params});
   if(url.pathname==='/api/enable-all')return json({message:'broadcast_sent'},202);
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
 if(url.pathname==='/api/status')return json({autoQueriesEnabled,id:Number(url.searchParams.get('id')),canReady:true,online:true,enabled:true,state:'idle',positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:true,fault:'none',homeOutcome:'none'});
 if(url.pathname==='/api/trace')return json({uptimeMs:10000,sequence:seq,frames:Array.from({length:30},(_,i)=>({seq:i+1,atMs:9000+i,dir:'RX',id:(i%2 ? 3 : 1)<<8,extended:true,remote:false,data:[0x3b,3,0x6b]}))});
 return json({});
});

try {
 await page.goto('http://queue.test/');
 await page.getByRole('tab',{name:'编排队列',exact:true}).click();
 await page.waitForFunction(()=>document.querySelectorAll('.trace__body .trace__row').length===30);
 await page.getByRole('button',{name:'全部使能',exact:true}).click();
 await page.waitForFunction(()=>document.body.textContent.includes('全部使能广播已发送'));
 await page.getByRole('button',{name:'全部失能',exact:true}).click();
 await page.waitForFunction(()=>document.body.textContent.includes('全部失能广播已发送'));
 assert.deepEqual(posts.filter(p=>p.path==='/api/enable-all').map(p=>p.params.enabled),['1','0']);
 await page.getByRole('button',{name:'暂停自动查询',exact:true}).click();
 await page.waitForTimeout(600);
 const count=gets;
 await page.waitForTimeout(2700);
 assert.ok(gets>count,'RX and HTTP reads must continue'); assert.equal(autoQueriesEnabled,false,'CAN automatic queries disabled');
 await page.getByRole('button',{name:'展开记录',exact:true}).click();
 await page.getByLabel('筛选电机 ID').fill('3');
 await page.getByRole('button',{name:'回零相关',exact:true}).click();
 assert.equal(await page.locator('.trace__body .trace__row').count(),15);
 await page.locator('.trace__detail summary').first().click();
 assert.equal(await page.locator('details[open]').count(),1);
 for (const size of [{width:1513,height:1039},{width:1280,height:800}]) {
   await page.setViewportSize(size);
   const disableBox=await page.getByRole('button',{name:'全部失能',exact:true}).boundingBox();
   assert.ok(disableBox.x+disableBox.width<=size.width);
   const box=await page.locator('.trace--expanded').boundingBox();
   assert.ok(box.height>500 && box.y+box.height<=size.height);
   await page.screenshot({path:`qa-trace-${size.width}.png`});
 }
 await page.keyboard.press('Escape');
 assert.equal(await page.locator('.trace--expanded').count(),0);
 await page.getByRole('button',{name:'恢复自动查询',exact:true}).click();
 await page.waitForTimeout(650);
 assert.ok(gets>count);
 assert.deepEqual(errors,[]);
 console.log('PASS trace expand, ID/home filter, full details, pause/resume CAN query setting while HTTP receives continue at both desktop sizes');
} finally {await browser.close();}
