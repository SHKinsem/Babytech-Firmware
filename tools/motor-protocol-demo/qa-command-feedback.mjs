import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFile } from 'node:fs/promises';

const require = createRequire(import.meta.url);
const { chromium } = require('C:/Users/xusen/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright');
const browser = await chromium.launch({headless:true,executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe'});
const page = await browser.newPage({viewport:{width:1280,height:800}});
const html = await readFile(new URL('../../device-controller/data/index.html',import.meta.url),'utf8');
const errors = [];
const frames = [];
let sequence = 0;
let rejectNext = false;
const config = [0x42,0x25,0x18,0x00,0x01,0x01,0x02,0x02,0x00,0x10,0x01,0x00,0x00,0x04,0xb0,0x0b,0x80,0x0b,0xb8,0x03,0xe8,0x05,0x07,0x00,0x01,0x00,0x01,0x00,0x08,0x08,0x98,0x07,0xd0,0x00,0x08,0x6b];
page.on('pageerror',error => errors.push(error.message));
page.route('http://feedback.test/**', async route => {
  const path = new URL(route.request().url()).pathname;
  const json = (body,status=200) => route.fulfill({status,contentType:'application/json',body:JSON.stringify(body)});
  if (path === '/') return route.fulfill({contentType:'text/html',body:html});
  if (route.request().method() === 'POST') {
    if (path === '/api/command') {
      if (rejectNext) {rejectNext=false;return json({error:'bad_request'},400);}
      const hex = new URLSearchParams(route.request().postData()).get('hex');
      const opcode = Number.parseInt(hex.slice(2,4),16);
      frames.push({seq:++sequence,atMs:1000+sequence,dir:'TX',id:0x100,extended:true,remote:false,data:[opcode,0x6b]});
      if (opcode === 0x24) frames.push({seq:++sequence,atMs:1000+sequence,dir:'RX',id:0x100,extended:true,remote:false,data:[0x24,0x5c,0x67,0x6b]});
      if (opcode === 0x1a) frames.push({seq:++sequence,atMs:1000+sequence,dir:'RX',id:0x100,extended:true,remote:false,data:[0x1a,0x01,0x6b]});
      if (opcode === 0xf3) frames.push({seq:++sequence,atMs:1000+sequence,dir:'RX',id:0x100,extended:true,remote:false,data:[0xf3,0x02,0x6b]});
      if (opcode === 0x42) for (let index=0;index<5;index++)
        frames.push({seq:++sequence,atMs:1000+sequence,dir:'RX',id:0x100+index,extended:true,remote:false,data:[0x42,...config.slice(1+index*7,1+(index+1)*7)]});
    }
    return json({message:'queued'},202);
  }
  if (path === '/api/status') return json({id:1,enabled:false,driverEnabled:false,online:true,canReady:true,state:'disabled',busState:'running',positionDeg:0,speedRpm:0,currentMa:0,lastAck:'none',fault:'none'});
  if (path === '/api/limits') return json({maxSpeedRpm:3000,maxAccelRpmS:2400,maxCurrentMa:5000,maxAngleDeg:360000,maxMoveSeconds:60,experimentSeconds:10});
  if (path === '/api/queue') return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,raw:false});
  if (path === '/api/trace') return json({uptimeMs:2000,sequence,frames});
  return json({});
});
try {
  await page.goto('http://feedback.test/');
  await page.getByText('设备在线',{exact:true}).waitFor();
  await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
  await page.getByLabel('搜索指令名称或功能码').fill('24');
  await page.locator('.library__item').first().click();
  await page.getByRole('button',{name:'发送指令',exact:true}).click();
  const result = page.getByRole('status',{name:'本次指令反馈'});
  await result.getByText(/23655 mV/).waitFor({timeout:4000});
  assert.match(await result.innerText(),/已入队/);
  await page.getByLabel('搜索指令名称或功能码').fill('42');
  await page.locator('.library__item').filter({hasText:'配置读取 Conf'}).click();
  await page.getByRole('button',{name:'发送指令',exact:true}).click();
  await result.getByText(/闭环模式最大电流 2944 mA/).waitFor({timeout:4000});
  assert.match(await result.innerText(),/收到 5 包/);
  await page.getByLabel('搜索指令名称或功能码').fill('1A');
  await page.locator('.library__item').first().click();
  await page.getByRole('button',{name:'发送指令',exact:true}).click();
  await result.getByText(/已收到原始回包：1A 01 6B/).waitFor({timeout:4000});
  await page.getByLabel('搜索指令名称或功能码').fill('F3');
  await page.locator('.library__item').first().click();
  await page.getByRole('button',{name:'发送指令',exact:true}).click();
  await result.getByText(/接收的命令正确/).waitFor({timeout:4000});
  rejectNext = true;
  await page.getByLabel('搜索指令名称或功能码').fill('24');
  await page.locator('.library__item').first().click();
  await page.getByRole('button',{name:'发送指令',exact:true}).click();
  await result.getByText(/板端拒绝：/).waitFor({timeout:4000});
  assert.doesNotMatch(await result.innerText(),/23655 mV/,'old RX must not become the rejected request result');
  if (process.env.QA_SCREENSHOT) await page.screenshot({path:process.env.QA_SCREENSHOT});
  assert.deepEqual(errors,[]);
  console.log('PASS command response is visible beside send with submitted and actual RX states');
} finally { await browser.close(); }
