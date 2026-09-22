// Desktop QA for the local form drafts of the device page.
//
// Conventions follow qa-current-limit.mjs: the built device page
// (motion/data/index.html) is served behind a mocked board API and driven in a
// real Chromium. Every request the page makes is recorded, so "restoring a draft
// never sends anything" is an assertion about real traffic, and the browser's
// localStorage is inspected directly.
//
// Run it from this folder after the device page has been rebuilt:
//   node qa-drafts.mjs
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
const DRAFTS_KEY='babytech.device-drafts', QUEUE_DRAFT_KEY='motor-protocol-demo.queue.draft.v1';
// Disabled, stationary node with confirmed limits: every command in this run is
// a parameter write, so nothing here needs an enable.
const motor={enabled:false,state:'disabled',online:true,canReady:true,busState:'running',txErrors:0,
  positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:false,lastAck:'received',fault:'none',
  activeId:0,homeOutcome:'none',homeId:0,homeMode:null,homeOrg:null,homeRunning:null,homeFailed:null};
let limitsCurrent=5000;
const html=await readFile(new URL('../../motion/data/index.html',import.meta.url),'utf8');
await page.route('http://device.test/**',async route=>{
  const url=new URL(route.request().url()), path=url.pathname;
  const json=body=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/favicon.ico')return route.fulfill({status:204});
  if(path==='/api/config-result')return json({sequence:0});
  if(path==='/api/logs')return json({bootId:'QABOOT',uptimeMs:1,sequence:0,capacity:48,events:[]});
  if(route.request().method()==='POST'){
    posts.push({path,params:Object.fromEntries(new URLSearchParams(route.request().postData()))});
    return json({ok:true,message:'queued'});
  }
  if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:limitsCurrent,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
  if(path==='/api/status')return json({...motor,id:Number(url.searchParams.get('id'))});
  if(path==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
  if(path==='/api/trace')return json({uptimeMs:12000,sequence:1,frames:[]});
  throw Error('Unexpected asset/API '+path);
});
const field=(id)=>page.locator(id);
const stored=async()=>page.evaluate((key)=>localStorage.getItem(key),DRAFTS_KEY);
const parsed=async()=>JSON.parse(await stored() ?? '{}');
const setMotorId=async(value)=>{
  await field('input[aria-label="CAN ID"]').fill(value);
  await page.waitForTimeout(120);
};
const selectCommand=async(query)=>{
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByLabel('搜索指令名称或功能码').fill(query);
  await page.locator('.library__item').first().click();
};
try {
  await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
  await page.goto('http://device.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  assert.equal(posts.length,0);checks.push('加载网页没有自动 POST');
  assert.equal(await stored(),null,'a fresh page writes no drafts until something is edited');
  // The queued program keeps its own storage: seed it and check it never moves.
  await page.evaluate((key)=>localStorage.setItem(key,'{"program":"move 1 90","repeat":1}'),QUEUE_DRAFT_KEY);

  // 1. Edits in the homing parameters and in the closed-loop current write.
  await selectCommand('原点参数配置');
  await field('#field-vel').fill('120');
  await page.getByRole('radio',{name:'无限位碰撞',exact:true}).click();
  await selectCommand('闭环最大相电流');
  await field('#field-currentMa').fill('200');

  // 2. Going back restores exactly what was typed (and not the other command's).
  await selectCommand('原点参数配置');
  assert.equal(await field('#field-vel').inputValue(),'120');
  assert.equal(await page.getByRole('radio',{name:'无限位碰撞',exact:true}).getAttribute('aria-checked'),'true');
  await selectCommand('闭环最大相电流');
  assert.equal(await field('#field-currentMa').inputValue(),'200');
  checks.push('指令来回切换后原样恢复：4C 回零速度 120、模式 2，以及 0x45 的 200 mA');

  // 3. Variants of one command are saved independently.
  await selectCommand('直通位置');
  await field('#field-vel').fill('300');
  await page.getByRole('radio',{name:'CB 限流',exact:true}).click();
  await field('#field-vel').fill('600');
  await page.getByRole('radio',{name:'FB 基础',exact:true}).click();
  assert.equal(await field('#field-vel').inputValue(),'300');
  await page.getByRole('radio',{name:'CB 限流',exact:true}).click();
  assert.equal(await field('#field-vel').inputValue(),'600');
  checks.push('FB 与 CB 变体各自保存：FB 300、CB 600，互不覆盖');

  // 4. Motors are isolated, and an invalid intermediate id changes nothing.
  await selectCommand('闭环最大相电流');
  await field('#field-currentMa').fill('201');
  await setMotorId('2');
  await field('#field-currentMa').fill('999');
  await setMotorId('');
  assert.equal(await field('#field-currentMa').inputValue(),'999','a partial id never resets the form');
  assert.equal((await parsed()).forms['1|closedLoopCurrentLimit|base'].values.currentMa,'201','motor 1 draft untouched');
  await setMotorId('1');
  assert.equal(await field('#field-currentMa').inputValue(),'201');
  await setMotorId('2');
  assert.equal(await field('#field-currentMa').inputValue(),'999');
  await setMotorId('1');
  checks.push('电机 1／2 的草稿互相隔离，中间输入非法 CAN ID 不会覆盖已有草稿');

  // 5. Trial fields are drafts too, also per motor, and survive a tab change.
  await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  await field('#manual-angle').fill('120');
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  assert.equal(await field('#manual-angle').inputValue(),'120');
  await setMotorId('2');
  assert.equal(await field('#manual-angle').inputValue(),'10','motor 2 has its own trial draft');
  await field('#manual-angle').fill('15');
  await setMotorId('1');
  assert.equal(await field('#manual-angle').inputValue(),'120');
  checks.push('常规试动参数按电机保存，切标签页回来仍是 120，电机 2 独立');

  // 6. A reload keeps the form, the unsent HEX draft and the open command.
  await selectCommand('原点参数配置');
  await field('#field-vel').fill('121');
  await page.getByRole('tab',{name:'原始 HEX',exact:true}).click();
  const rawHex='01 4C AE 01 02 00 00 1E 00 00 27 10 01 2C 03 E8 00 3C 00 6B';
  await field('#raw-command').fill(rawHex);
  await page.reload();
  await page.getByText('设备在线',{exact:true}).waitFor();
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  assert.equal(await page.getByRole('heading',{name:'原点参数配置'}).count(),1);
  assert.equal(await page.getByRole('tab',{name:'原始 HEX',exact:true}).getAttribute('aria-selected'),'true');
  assert.equal(await field('#raw-command').inputValue(),rawHex,'the unsent HEX draft survives a reload');
  await page.getByRole('tab',{name:'参数编辑',exact:true}).click();
  assert.equal(await field('#field-vel').inputValue(),'121');
  await page.getByRole('tab',{name:'原始 HEX',exact:true}).click();
  assert.equal(await field('#raw-command').inputValue(),rawHex,'returning to the form does not drop it');
  // The raw draft is a draft: it was never submitted.
  assert.equal(posts.length,0);
  checks.push('刷新后恢复指令、表单值与未发送的 HEX 草稿，且从未提交任何帧');

  // The variant last used for a command comes back too, not just the default one.
  await selectCommand('直通位置');
  assert.equal(await page.getByRole('radio',{name:'CB 限流',exact:true}).getAttribute('aria-checked'),'true');
  assert.equal(await field('#field-vel').inputValue(),'600');
  checks.push('刷新后回到该指令上次使用的变体（CB 限流 600），而不是默认变体');

  // 8. Nothing about the board is stored, and the queue draft is untouched.
  const blob=await stored();
  for(const forbidden of ['password','ssid','"enabled"','"online"','maxSpeedRpm','"fault"','lastAck','token'])
    assert.doesNotMatch(blob,new RegExp(forbidden),forbidden);
  assert.equal(await page.evaluate((key)=>localStorage.getItem(key),QUEUE_DRAFT_KEY),'{"program":"move 1 90","repeat":1}');
  checks.push('本地草稿只含可编辑输入，不含密码／使能／在线／限制／回包等板端状态；编排队列草稿未被改动');

  // 9. A corrupt payload and a denied storage must not break the page.
  await page.evaluate((key)=>localStorage.setItem(key,'{not json'),DRAFTS_KEY);
  await page.reload();
  await page.getByText('设备在线',{exact:true}).waitFor();
  await selectCommand('闭环最大相电流');
  assert.equal(await field('#field-currentMa').inputValue(),'120','corrupt drafts fall back to defaults');
  assert.equal(await page.getByRole('heading',{name:'闭环最大相电流'}).count(),1);
  await field('#field-currentMa').fill('150');
  assert.equal((await parsed()).forms['1|closedLoopCurrentLimit|base'].values.currentMa,'150');
  checks.push('草稿内容损坏时回退到默认值，页面照常工作');

  const deniedPage=await browser.newPage({viewport:{width:1513,height:1039}});
  const deniedErrors=[];deniedPage.on('pageerror',e=>deniedErrors.push(e.message));
  await deniedPage.route('http://device.test/**',async route=>{
    const path=new URL(route.request().url()).pathname;
    if(path==='/')return route.fulfill({contentType:'text/html',body:html});
    if(path==='/favicon.ico')return route.fulfill({status:204});
    if(path==='/api/config-result')return route.fulfill({status:200,contentType:'application/json',body:'{"sequence":0}'});
    if(path==='/api/limits')return route.fulfill({status:200,contentType:'application/json',body:'{"maxSpeedRpm":120,"maxAccelRpmS":240,"maxCurrentMa":5000,"maxAngleDeg":3600,"maxMoveSeconds":60,"experimentSeconds":5}'});
    if(path==='/api/status')return route.fulfill({status:200,contentType:'application/json',body:JSON.stringify({...motor,id:1})});
    if(path==='/api/queue')return route.fulfill({status:200,contentType:'application/json',body:'{"state":"idle","runId":0,"step":0,"total":0,"iteration":0,"repeat":1,"line":0,"action":"","message":"","raw":false}'});
    if(path==='/api/trace')return route.fulfill({status:200,contentType:'application/json',body:'{"uptimeMs":12000,"sequence":1,"frames":[]}'});
    return route.fulfill({status:200,contentType:'application/json',body:'{}'});
  });
  await deniedPage.addInitScript(()=>{
    Object.defineProperty(window,'localStorage',{configurable:true,get(){throw new Error('denied');}});
  });
  await deniedPage.goto('http://device.test/');
  await deniedPage.getByText('设备在线',{exact:true}).waitFor();
  await deniedPage.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await deniedPage.getByLabel('搜索指令名称或功能码').fill('闭环最大相电流');
  await deniedPage.locator('.library__item').first().click();
  await deniedPage.locator('#field-currentMa').fill('180');
  assert.equal(await deniedPage.locator('#field-currentMa').inputValue(),'180','in-memory drafts still work');
  assert.deepEqual(deniedErrors,[]);
  await deniedPage.close();
  checks.push('浏览器拒绝 localStorage 时页面不崩溃，草稿退化为内存内保留');

  // 10. A restored draft is validated by the board limits, never clamped.
  limitsCurrent=500;
  await page.evaluate(([key,payload])=>localStorage.setItem(key,payload),[DRAFTS_KEY,JSON.stringify({
    version:1,seq:3,lastMotorId:'1',lastCommandId:'closedLoopCurrentLimit',lastVariantKey:'base',
    forms:{'1|closedLoopCurrentLimit|base':{values:{save:0,currentMa:'900'},mode:'form',raw:'',dirty:false,used:3}},
    manuals:{},
  })]);
  await page.reload();
  await page.getByText('设备在线',{exact:true}).waitFor();
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByText(/超出板端当前上限 500 mA/).waitFor();
  assert.equal(await field('#field-currentMa').inputValue(),'900','the draft is shown as typed');
  assert.equal(await page.getByRole('button',{name:'发送指令',exact:true}).isDisabled(),true);
  assert.match(await page.locator('.actions__gate').innerText(),/闭环最大相电流 900 mA 超出板端当前上限 500 mA/);
  assert.equal(posts.length,0);
  checks.push('恢复的草稿按当前板端限制校验后被拦下，没有被静默裁剪');

  await page.screenshot({path:fileURLToPath(new URL('./qa/drafts-lab-1513.png',import.meta.url)),fullPage:true});
  assert.equal(posts.length,0);checks.push('整个流程没有自动发送任何指令');
  assert.deepEqual(errors,[]);checks.push('无浏览器运行错误');
  await writeFile(new URL('./qa/drafts-results.json',import.meta.url),JSON.stringify({checks,errors,posts},null,2));
  console.log(JSON.stringify({passed:checks.length,checks},null,2));
} finally {await browser.close();}
