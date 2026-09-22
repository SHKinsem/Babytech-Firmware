// Real embedded brain page, mocked HTTP only. Never accesses a hardware board.
const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');
let playwright;
try { playwright=require(process.env.PLAYWRIGHT_MODULE || 'playwright'); }
catch { playwright=require('C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright'); }
(async()=>{
 const browser=await playwright.chromium.launch({headless:true,executablePath:process.env.BROWSER_EXECUTABLE ||
   (process.platform==='win32'?'C:/Program Files/Google/Chrome/Application/chrome.exe':undefined)});
 try {
  const page=await browser.newPage({viewport:{width:390,height:844}});
  const errors=[],posts=[];page.on('pageerror',e=>errors.push(e.message));
  let state={connected:true,boot:'0000000000000009',revision:1,parameterRevision:1,
   motionState:'idle',motionUptimeMs:500,responses:5,motorFlags:3,busy:false,
   command:'none',reason:0,params:{motor:1,angleTenths:100,speedTenths:50,accel:10,decel:10,current:800}};
  const html=fs.readFileSync(path.join(__dirname,'../brain/data/index.html'),'utf8');
  await page.route('http://brain.test/**',async route=>{
   const url=new URL(route.request().url());
   if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:html});
   if(url.pathname==='/favicon.ico')return route.fulfill({status:204});
   if(route.request().method()==='POST') {
    const params=Object.fromEntries(new URLSearchParams(route.request().postData()));posts.push({path:url.pathname,params});
    state={...state,busy:true,command:'waiting'};
    return route.fulfill({status:202,contentType:'application/json',body:'{"queued":true}'});
   }
   return route.fulfill({contentType:'application/json',body:JSON.stringify(state)});
  });
  await page.goto('http://brain.test/');
  await page.waitForFunction(()=>!document.getElementById('apply').disabled);
  assert.equal(posts.length,0);
  await page.locator('[name="speedTenths"]').fill('100');
  assert(await page.locator('#run').isDisabled());
  await page.locator('#apply').click();
  await page.waitForFunction(()=>document.getElementById('command-result').textContent.includes('等待'));
  assert.equal(posts[0].path,'/api/params');assert.equal(posts[0].params.speedTenths,'100');
  assert.equal(posts[0].params.revision,'1');assert.equal(posts[0].params.boot,state.boot);
  state={...state,busy:false,command:'ok',revision:2,parameterRevision:2,params:{...state.params,speedTenths:100}};
  await page.waitForFunction(()=>!document.getElementById('run').disabled);
  await page.locator('#run').click();
  assert.equal(posts.at(-1).params.revision,'2');assert.equal(posts.at(-1).params.action,'run');
  state={...state,busy:true,command:'accepted',motionState:'running'};
  await page.waitForFunction(()=>document.getElementById('command-result').textContent.includes('等待执行结果'));
  assert(await page.locator('#run').isDisabled());assert(!(await page.locator('#stop').isDisabled()));
  await page.locator('#stop').click();assert.equal(posts.at(-1).path,'/api/stop');
  state={...state,busy:false,command:'failed',reason:13};
  await page.waitForFunction(()=>document.getElementById('command-result').textContent.includes('停止未确认'));
  state={...state,connected:false,busy:true,command:'unknown'};
  await page.waitForFunction(()=>document.getElementById('command-result').textContent.includes('结果未知'));
  assert(await page.locator('#enable').isDisabled());assert(!(await page.locator('#stop').isDisabled()));
  state={...state,connected:true,busy:false,command:'unknown_after_restart',reason:12,boot:'000000000000000a',
    revision:1,parameterRevision:1,params:{...state.params,speedTenths:50}};
  await page.waitForFunction(()=>document.querySelector('[name="speedTenths"]').value==='50');
  assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth));
  assert.deepEqual(errors,[]);
  console.log('PASS: brain UI no auto motion, parameter revision, dirty form, async result, stop, offline and restart');
 } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
