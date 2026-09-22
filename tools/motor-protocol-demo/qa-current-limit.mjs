// Desktop QA for the closed-loop maximum phase current write (0x45, manual
// V1.0.5 p82 / 5.6.13).
//
// Same conventions as qa-direct-position.mjs: the built device page
// (motion/data/index.html) is served behind a mocked board API, the page is
// driven in a real Chromium and every request it makes is recorded, so the
// assertions are about the exact bytes submitted and about what the page does
// NOT claim. Nothing here talks to hardware.
//
// Run it from this folder after the device page has been rebuilt:
//   node qa-current-limit.mjs
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
const errors=[], posts=[], checks=[];
page.on('pageerror',e=>errors.push(e.message));
// The state a 0x45 write needs: driver disabled, fresh stationary feedback.
// The page must not need an enable confirmation for this parameter write.
const motor={enabled:false,state:'disabled',online:true,canReady:true,busState:'running',txErrors:0,
  positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:false,lastAck:'received',fault:'none',
  activeId:0,homeOutcome:'none',homeId:0,homeMode:null,homeOrg:null,homeRunning:null,homeFailed:null};
let limitsOk=true;
const html=await readFile(new URL('../../motion/data/index.html',import.meta.url),'utf8');
await page.route('http://device.test/**',async route=>{
  const url=new URL(route.request().url()), path=url.pathname;
  const json=body=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/favicon.ico')return route.fulfill({status:204});
  if(route.request().method()==='POST'){
    const params=Object.fromEntries(new URLSearchParams(route.request().postData()));
    posts.push({path,params});
    // One answer per submission: the page must not repair a request by resending.
    return json({ok:true,message:'queued'});
  }
  if(path==='/api/limits')return limitsOk
    ? json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5})
    : route.fulfill({status:404,contentType:'application/json',body:JSON.stringify({error:'not found'})});
  if(path==='/api/status')return json({...motor,id:Number(url.searchParams.get('id'))});
  if(path==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
  if(path==='/api/trace')return json({uptimeMs:12000,sequence:1,frames:[]});
  throw Error('Unexpected asset/API '+path);
});
const selectCommand=async(query)=>{
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByLabel('搜索指令名称或功能码').fill(query);
  await page.locator('.library__item').first().click();
};
try {
  await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
  await page.goto('http://device.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  assert.equal(posts.length,0);checks.push('加载网页没有自动 POST，也没有模拟数据');

  await selectCommand('闭环最大相电流');
  const send=page.getByRole('button',{name:'发送指令',exact:true});
  assert.equal(await page.getByRole('heading',{name:'闭环最大相电流'}).count(),1);
  // The defaults are editor examples, not a value read back from the driver.
  assert.equal(await page.locator('#field-currentMa').inputValue(),'120');
  assert.equal(await page.getByRole('radio',{name:'不保存',exact:true}).getAttribute('aria-checked'),'true');
  const note=await page.locator('.panel--command .info-line').innerText();
  assert.match(note,/全局闭环最大相电流/);
  assert.match(note,/碰撞检测阈值/);
  assert.match(note,/示例/);
  assert.match(note,/关闭使能/);
  // A parameter write needs confirmed limits, but no enable.
  assert.equal(await page.getByText('请先发送使能，并等待真实确认').count(),0);
  assert.equal(await send.isDisabled(),false);
  await send.click();
  await page.waitForTimeout(120);
  assert.deepEqual(posts.at(-1),{path:'/api/command',params:{hex:'0145660000786B'}});
  checks.push('0x45 默认示例 120 mA 按手册字节发送：01 45 66 00 00 78 6B');

  // The page must not present the submission as a confirmed driver setting.
  const notice=await page.locator('.panel--feedback').innerText();
  assert.match(notice,/等待真实反馈/);
  assert.doesNotMatch(notice,/已生效|设置成功|已设置为/);
  checks.push('提交后只报告已入队，不伪造已生效的电流设置');

  // Saving switches the frame to the manual's saved example (100 mA).
  await page.getByRole('radio',{name:'保存',exact:true}).click();
  await page.locator('#field-currentMa').fill('100');
  await page.locator('.code-box').first().waitFor();
  assert.match(await page.locator('.code-box').first().innerText(),/01 45 66 01 00 64 6B/);
  await send.click();
  await page.waitForTimeout(120);
  assert.deepEqual(posts.at(-1),{path:'/api/command',params:{hex:'0145660100646B'}});
  const sentOnce=posts.length;
  await page.waitForTimeout(800);
  assert.equal(posts.length,sentOnce);
  assert.equal(posts.filter(p=>p.path==='/api/command').length,2);
  checks.push('保存选项改变预览与实际提交：01 45 66 01 00 64 6B，且每次操作只发送一次');

  await page.screenshot({path:fileURLToPath(new URL('./qa/current-limit-lab-1513.png',import.meta.url)),fullPage:true});

  // Without confirmed limits the write is gated: its value cannot be checked.
  limitsOk=false;
  await page.goto('http://device.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  await selectCommand('闭环最大相电流');
  await page.waitForTimeout(100);
  assert.equal(await page.getByRole('button',{name:'发送指令',exact:true}).isDisabled(),true);
  assert.match(await page.locator('.actions__gate').innerText(),/未读取到板端限制/);
  assert.equal(posts.filter(p=>p.path==='/api/command').length,2);
  checks.push('未读取到 /api/limits 时 0x45 被禁用，且不会提交任何帧');

  assert.deepEqual(errors,[]);checks.push('无浏览器运行错误');
  await writeFile(new URL('./qa/current-limit-results.json',import.meta.url),JSON.stringify({checks,errors,posts},null,2));
  console.log(JSON.stringify({passed:checks.length,checks},null,2));
} finally {await browser.close();}
