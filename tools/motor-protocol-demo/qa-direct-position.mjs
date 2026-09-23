// Desktop QA for the supervised direct (FB/CB) position commands.
//
// Same conventions as qa-device.mjs: the built device page (device-controller/data/index.html)
// is served behind a mocked board API, the page is driven in a real Chromium, and
// every request the page makes is recorded so the assertions are about actual
// submitted bytes and nothing else. Nothing here talks to hardware.
//
// Run it from this folder after the device page has been rebuilt:
//   node qa-direct-position.mjs
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
// The board's answer for one addressed node: confirmed enable, fresh stationary
// feedback and a fresh 0x33 target sample (p70), so only the command under test can
// decide whether a send is allowed.
const motor={enabled:true,state:'idle',online:true,canReady:true,busState:'running',txErrors:0,
  positionDeg:100.0,speedRpm:0,currentMa:0,targetDeg:200.0,driverEnabled:true,lastAck:'received',fault:'none',
  activeId:0,homeOutcome:'none',homeId:0,homeMode:null,homeOrg:null,homeRunning:null,homeFailed:null};
const html=await readFile(new URL('../../device-controller/data/index.html',import.meta.url),'utf8');
await page.route('http://device.test/**',async route=>{
  const url=new URL(route.request().url()), path=url.pathname;
  const json=body=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/favicon.ico')return route.fulfill({status:204});
  if(path==='/api/config-result')return json({sequence:0});
  if(path==='/api/logs')return json({bootId:'QABOOT',uptimeMs:1,sequence:0,capacity:48,events:[]});
  if(route.request().method()==='POST'){
    const params=Object.fromEntries(new URLSearchParams(route.request().postData()));
    posts.push({path,params});
    // Exactly one answer per submission: the page must never repair a request by
    // resending it, so a duplicate would show up in `posts`.
    return json({ok:true,message:'queued',id:Number(params.id||url.searchParams.get('id')||0)});
  }
  if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
  if(path==='/api/status')return json({...motor,id:Number(url.searchParams.get('id'))});
  if(path==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
  if(path==='/api/trace')return json({uptimeMs:20000,sequence:1,frames:[]});
  throw Error('Unexpected asset/API '+path);
});
try {
  await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
  await page.goto('http://device.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  assert.equal(posts.length,0);checks.push('加载网页没有自动 POST，也没有模拟数据');

  // The documented case: motor 2, FB, CW, 300 (0.1 RPM), 900 (0.1 degree), mode 2.
  await page.getByLabel('CAN ID',{exact:true}).fill('2');
  await page.waitForTimeout(550);
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByLabel('搜索指令名称或功能码').fill('直通位置');
  await page.locator('.library__item').first().click();
  const send=page.getByRole('button',{name:'发送指令',exact:true});
  assert.equal(await page.getByRole('heading',{name:'直通位置'}).count(),1);
  assert.equal(await page.getByRole('radio',{name:'CW',exact:true}).getAttribute('aria-checked'),'true');
  assert.equal(await page.getByRole('radio',{name:'相对当前位置',exact:true}).getAttribute('aria-checked'),'true');
  // Field inputs are addressed by their id: the label text alone also matches
  // the HelpTip button that carries the same accessible name.
  const speedField=page.locator('#field-vel'), angleField=page.locator('#field-clk');
  await speedField.fill('300');
  await angleField.fill('900');
  assert.equal(await send.isDisabled(),false);
  assert.match(await page.locator('.actions__gate, .actions__hint').first().innerText(),/收到应答不等于机械动作完成|等待/);
  await send.click();
  await page.waitForTimeout(120);
  assert.deepEqual(posts.at(-1),{path:'/api/command',params:{hex:'02FB00012C0000038402006B'}});
  checks.push('FB 直通位置按原功能码与字节发送：02 FB 00 012C 00000384 02 00 6B');

  // CB is the same command plus the current field after sync: 800 mA here.
  await page.getByRole('radio',{name:'CB 限流',exact:true}).click();
  assert.equal(await page.getByRole('heading',{name:'直通位置'}).count(),1);
  await speedField.fill('300');
  await angleField.fill('900');
  await page.locator('#field-maxCurrentMa').fill('800');
  await send.click();
  await page.waitForTimeout(120);
  assert.deepEqual(posts.at(-1),{path:'/api/command',params:{hex:'02CB00012C00000384020003206B'}});
  checks.push('CB 直通位置保留 CB 功能码并在同步位后追加最大电流：02 CB 00 012C 00000384 02 00 0320 6B');

  // Nothing is retried on its own once the request was answered.
  const sentOnce=posts.length;
  await page.waitForTimeout(900);
  assert.equal(posts.length,sentOnce);
  assert.equal(posts.filter(p=>p.path==='/api/command').length,2);checks.push('两次提交各发送一次，页面不会自动重发');

  // The cached (sync 1) form stays gated even though the rest of the frame is
  // valid; the page must not offer it.
  await page.getByRole('radio',{name:'缓存待同步',exact:true}).click();
  assert.equal(await send.isDisabled(),true);
  assert.match(await page.locator('.actions__gate').innerText(),/同步标志必须为 0/);
  await page.getByRole('radio',{name:'立即执行',exact:true}).click();
  assert.equal(await send.isDisabled(),false);checks.push('同步缓存方式仍被拦截，切回立即执行后恢复可发送');

  // Mode 0 is no longer preview-only: the page allows it and says that the
  // baseline comes from the driver's target position read (0x33, manual p70),
  // resolved on the board - and that the p71 setpoint is not a substitute.
  await page.getByRole('radio',{name:'相对上一目标',exact:true}).click();
  assert.equal(await send.isDisabled(),false);
  const note=await page.getByText(/直通位置（FB\/CB）由板端解析/).innerText();
  assert.match(note,/0x33/);
  assert.match(note,/0x34 实时设定值/);checks.push('模式 0 已可用，并显式标注基准由板端按 0x33（p70）新鲜读值解析、0x34 不可代替');

  // A mode-2 zero-speed command is refused by the page exactly like the board.
  await page.getByRole('radio',{name:'相对当前位置',exact:true}).click();
  await speedField.fill('0');
  assert.equal(await send.isDisabled(),true);
  assert.match(await page.locator('.actions__gate').innerText(),/零位移/);
  await angleField.fill('0');
  assert.equal(await send.isDisabled(),false);checks.push('速度为 0 只在零位移空操作时允许，页面与板端判定一致');

  await page.screenshot({path:fileURLToPath(new URL('./qa/direct-position-lab-1513.png',import.meta.url)),fullPage:true});
  assert.deepEqual(errors,[]);checks.push('无浏览器运行错误');
  await writeFile(new URL('./qa/direct-position-results.json',import.meta.url),JSON.stringify({checks,errors,posts},null,2));
  console.log(JSON.stringify({passed:checks.length,checks},null,2));
} finally {await browser.close();}
