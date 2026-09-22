import {createRequire} from 'node:module';
import {readFile,mkdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';
const require=createRequire(import.meta.url);
const {chromium}=require('C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
const page=await browser.newPage({viewport:{width:1280,height:800}});
const html=await readFile(new URL('../../motion/data/index.html',import.meta.url),'utf8');
let limits={maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5};
let saveFail=false,limitsOffline=false,seq=1,commandFail=false;
const posts=[],errors=[];page.on('pageerror',e=>errors.push(e.message));
await page.route('http://limits.test/**',async route=>{
 const url=new URL(route.request().url());
 const json=(body,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
 if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
 if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
 if(route.request().method()==='POST'){
   const params=Object.fromEntries(new URLSearchParams(route.request().postData()));posts.push({path:url.pathname,params});
   if(url.pathname==='/api/limits'){
     if(saveFail)return json({error:'limits_save_failed'},500);
     limits=Object.fromEntries(Object.entries(params).map(([k,v])=>[k,Number(v)]));return json(limits);
   }
   if(commandFail){seq+=100;return json({error:'accel_out_of_range'},400);}
   return json({message:'queued_experiment'},202);
 }
 if(url.pathname==='/api/limits')return limitsOffline?route.abort():json(limits);
 if(url.pathname==='/api/status')return json({id:1,canReady:true,online:true,enabled:true,state:'idle',positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:true,fault:'none'});
 if(url.pathname==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
 if(url.pathname==='/api/trace')return json({uptimeMs:10000,sequence:seq,frames:[{seq,atMs:9990,dir:'RX',id:256,extended:true,data:[0x3a,3,0x6b]}]});
 return json({});
});
try {
 await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
 await page.goto('http://limits.test/');await page.getByText('设备在线',{exact:true}).waitFor();
 await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
 const search=page.getByLabel('搜索指令名称或功能码');await search.fill('F6');await page.locator('.library__item').first().click();
 await page.locator('#field-acc').fill('1000');await page.locator('#field-vel').fill('1000');
 const send=page.getByRole('button',{name:'发送指令',exact:true});
 assert.equal(await send.isDisabled(),true);await page.getByText(/加速度 1000.*240/).waitFor();
 assert.equal(posts.length,0);
 await page.getByRole('tab',{name:'调试限制',exact:true}).click();
 await page.locator('#limit-maxAccelRpmS').fill('2000');await page.locator('#limit-experimentSeconds').fill('0');
 await page.waitForTimeout(2200);
 assert.equal(await page.locator('#limit-maxAccelRpmS').inputValue(),'2000','poll must preserve unsaved draft');
 assert.equal(posts.length,0,'editing never saves');
 await page.getByRole('button',{name:'保存到板上',exact:true}).click();
 await page.getByText(/已保存到板端 NVS/).waitFor();
 assert.equal(limits.maxAccelRpmS,2000);assert.equal(limits.experimentSeconds,0);
 for(const width of [1280,1513]){
   await page.setViewportSize({width,height:width===1280?800:1039});
   assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
   await page.screenshot({path:fileURLToPath(new URL(`./qa/limits-${width}.png`,import.meta.url)),fullPage:true});
 }
 saveFail=true;await page.locator('#limit-maxAccelRpmS').fill('3000');
 await page.getByRole('button',{name:'保存到板上',exact:true}).click();
 await page.getByText(/保存失败.*NVS/).waitFor();assert.equal(limits.maxAccelRpmS,2000);
 await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
 assert.equal(await send.isEnabled(),true);
 await send.click();await page.getByText(/已入队，持续运行，手动停止/).waitFor();
 assert.equal(posts.at(-1).params.hex,'01F60003E803E8006B');
 commandFail=true;await send.click();
 await page.getByText(/板端拒绝.*HTTP 400/).waitFor();await page.waitForTimeout(750);
 assert.equal(await page.getByText(/板端拒绝.*HTTP 400/).count(),1,'trace loss must not overwrite rejection');
 assert.equal(await page.getByText(/超时或断线不代表/).count(),0);
 await page.getByText(/部分总线记录已被环形缓冲区覆盖/).waitFor();
 limitsOffline=true;await page.waitForTimeout(2600);assert.equal(await send.isDisabled(),true);
 assert.deepEqual(errors,[]);
 console.log('PASS: editable NVS limits workflow, screenshot command, continuous mode, no auto POST, dirty draft, failed save, persistent rejection, offline gate and desktop layouts (mock APIs).');
} finally {await browser.close();}
