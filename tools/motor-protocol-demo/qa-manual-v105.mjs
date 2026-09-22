import {createRequire} from 'node:module';
import {readFile,mkdir} from 'node:fs/promises';
import assert from 'node:assert/strict';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
const page=await browser.newPage({viewport:{width:1513,height:1039}});
const html=await readFile(new URL('../../motion/data/index.html',import.meta.url),'utf8');
const errors=[],posts=[];let seq=0,frames=[],online=true;
page.on('pageerror',e=>errors.push(e.message));
await page.route('http://manual.test/**',async route=>{
 const url=new URL(route.request().url());
 const json=body=>route.fulfill({contentType:'application/json',body:JSON.stringify(body)});
 if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
 if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
 if(!online)return route.abort();
 if(route.request().method()==='POST'){
   const data=Object.fromEntries(new URLSearchParams(route.request().postData()));posts.push(data);
   if(data.hex==='01396B')frames=[{seq:++seq,atMs:9990,dir:'RX',id:0x100,extended:true,remote:false,data:[0x39,0,25,0x6b]}];
   if(data.hex==='013C6B')frames=[{seq:++seq,atMs:9990,dir:'RX',id:0x100,extended:true,remote:false,data:[0x3c,0,0x33,0x6b]}];
   return json({message:'queued'});
 }
 if(url.pathname==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
 if(url.pathname==='/api/status')return json({id:Number(url.searchParams.get('id')),canReady:true,online:true,enabled:false,state:'disabled',positionDeg:0,speedRpm:0,currentMa:0});
 if(url.pathname==='/api/config-result')return json({sequence:0});
 if(url.pathname==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
 if(url.pathname==='/api/trace')return json({uptimeMs:10000,sequence:seq,frames});
 return json({});
});
try {
 await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
 await page.goto('http://manual.test/');
 await page.getByText('设备在线',{exact:true}).waitFor();
 assert.equal(posts.length,0);
 await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
 const search=page.getByLabel('搜索指令名称或功能码');
 await search.fill('39');await page.locator('.library__item').first().click();
 await page.getByRole('button',{name:'发送指令',exact:true}).click();
 await page.locator('.panel--feedback').getByText(/-25/).waitFor();
 assert.equal(posts.at(-1).hex,'01396B');
 for(const size of [{width:1513,height:1039},{width:1280,height:800}]) {
   await page.setViewportSize(size);
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   const box=await page.getByRole('button',{name:'发送指令',exact:true}).boundingBox();
   assert.ok(box && box.y+box.height<=size.height,'send button visible');
   await page.screenshot({path:new URL(`./qa/manual-v105-${size.width}.png`,import.meta.url).pathname.replace(/^\/(.:)/,'$1'),fullPage:true});
 }
 await page.getByLabel('CAN ID',{exact:true}).fill('2');await page.waitForTimeout(550);
 assert.equal(await page.locator('.panel--feedback').getByText(/-25/).count(),0,'different motor must not inherit reply');
 await page.getByLabel('CAN ID',{exact:true}).fill('1');
 await search.fill('3C');await page.locator('.library__item').first().click();
 await page.getByRole('button',{name:'发送指令',exact:true}).click();
 await page.waitForTimeout(750);
 const feedback=await page.locator('.panel--feedback').innerText();
 assert.match(feedback,/未.*回零|不在回零|未运行/);
 assert.doesNotMatch(feedback,/回零成功|已完成回零/);
 await search.fill('4C');await page.locator('.library__item').first().click();
 assert.equal(await page.getByRole('radiogroup',{name:'回零模式',exact:true}).getByRole('radio').count(),6);
 const panelBounds=await page.locator('.panel--command').boundingBox();
 for(const option of await page.getByRole('radiogroup',{name:'回零模式',exact:true}).getByRole('radio').all()) {
   const bounds=await option.boundingBox();assert.ok(bounds.x+bounds.width<=panelBounds.x+panelBounds.width,'homing options fit column');
 }
 await page.screenshot({path:new URL('./qa/manual-v105-homing-1280.png',import.meta.url).pathname.replace(/^\/(.:)/,'$1'),fullPage:true});
 online=false;await page.getByText('设备未连接',{exact:true}).waitFor();
 assert.equal(await page.locator('.panel--feedback').getByText(/-25/).count(),0);
 assert.deepEqual(errors,[]);
 console.log('PASS: V1.0.5 queries, decoded real-trace fixtures, target isolation, offline state, desktop viewports. APIs mocked; no hardware.');
} finally {await browser.close();}
