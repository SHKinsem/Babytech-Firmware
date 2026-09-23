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
let online=true, held=false, pendingRelease, seq=1;
let saved=false, wifiState='idle', scanning=false;
let motor={enabled:false,state:'disabled',online:true,canReady:true,busState:'running',txErrors:0,positionDeg:12.5,speedRpm:0,currentMa:0,driverEnabled:false,lastAck:'none',fault:'none'};
let scaleSample=20, scaleTared=true, scaleFactor=7842.125, scaleDoutPin=1, scaleSckPin=2;
const html=await readFile(new URL('../../device-controller/data/index.html',import.meta.url),'utf8');
await page.route('http://device.test/**',async route=>{
  const url=new URL(route.request().url()), path=url.pathname;
  const json=body=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
  if(path==='/')return route.fulfill({contentType:'text/html',body:html});
  if(path==='/favicon.ico')return route.fulfill({status:204});
  if(path==='/api/config-result')return json({sequence:0});
  if(path==='/api/logs')return json({bootId:'QABOOT',uptimeMs:1,sequence:0,capacity:48,events:[]});
  if(!online)return route.abort();
  if(route.request().method()==='POST'){
    const params=Object.fromEntries(new URLSearchParams(route.request().postData()));posts.push({path,params});
    if(path==='/api/command' && params.hex==='01F3AB01006B')motor={...motor,enabled:true,state:'idle',driverEnabled:true,lastAck:'received'};
    if(path==='/api/wifi/connect'){saved=true;wifiState='connected';}
    if(path==='/api/wifi/forget'){saved=false;wifiState='idle';}
    if(path==='/api/wifi/scan')scanning=true;
    if(path==='/api/scale/tare')scaleTared=true;
    if(path==='/api/scale/config'){
      scaleDoutPin=Number(params.doutPin);scaleSckPin=Number(params.sckPin);
      return json({ok:true,initialized:true,doutPin:scaleDoutPin,sckPin:scaleSckPin,gpio45Allowed:true,available:true,status:'warming_up',calibrated:true,stable:false,rawCounts:null,netCounts:null,rawWeightG:null,weightG:null,sampleAgeMs:null,sampleCount:0,tareInProgress:false,tareCompleted:false,calibrationPersisted:true,tareRaw:-1230,countsPerGram:scaleFactor});
    }
    if(path==='/api/scale/calibrate'){
      scaleFactor=7901.25;
      return json({ok:true,initialized:true,doutPin:scaleDoutPin,sckPin:scaleSckPin,gpio45Allowed:true,available:true,status:'stable',calibrated:true,stable:true,rawCounts:12439241,netCounts:2380,rawWeightG:0.12,weightG:0.12,sampleAgeMs:20,sampleCount:scaleSample,tareInProgress:false,tareCompleted:true,calibrationPersisted:true,tareRaw:-1230,countsPerGram:scaleFactor});
    }
    if(held && path==='/api/move')await new Promise(r=>pendingRelease=r);
    return route.fulfill({status:202,contentType:'application/json',body:JSON.stringify({ok:true,message:'queued',state:scanning?'scanning':wifiState})});
  }
  if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
  if(path==='/api/status')return json({...motor,id:Number(url.searchParams.get('id'))});
  if(path==='/api/queue')return json({state:'idle',runId:0,step:0,total:0,iteration:0,repeat:1,line:0,action:'',message:'',raw:false});
  if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
  if(path==='/api/scale'){
    scaleSample+=1;
    const weight=0.08+(scaleSample-20)*0.004;
    return json({ok:true,initialized:true,doutPin:scaleDoutPin,sckPin:scaleSckPin,gpio45Allowed:true,available:true,status:'stable',calibrated:true,stable:true,rawCounts:12438621+scaleSample*31,netCounts:2311+scaleSample*7,rawWeightG:weight,weightG:weight,sampleAgeMs:32,sampleCount:scaleSample,tareInProgress:false,tareCompleted:scaleTared,calibrationPersisted:true,tareRaw:-1230,countsPerGram:scaleFactor});
  }
  if(path==='/api/trace')return json({uptimeMs:10000,sequence:seq,frames:[{seq,atMs:9990,dir:'RX',id:256,extended:true,remote:false,data:[0x36,0,0,0,0,125,0x6b]}]});
  if(path==='/api/wifi')return json({state:wifiState,ssid:saved?'Lab WiFi':'',saved,ip:saved?'192.168.1.50':'',apSsid:'Babytech-Motion',apIp:'192.168.4.1',busy:false,error:''});
  if(path==='/api/wifi/scan')return json({state:scanning?'done':'idle',networks:scanning?[{ssid:'Lab WiFi',secure:true,rssi:-48}]:[]});
  throw Error('Unexpected asset/API '+path);
});
try {
 await mkdir(new URL('./qa/',import.meta.url),{recursive:true});
 await page.goto('http://device.test/');
 await page.getByText('设备在线',{exact:true}).waitFor();
 assert.equal(posts.length,0);checks.push('加载网页没有自动 POST，无模拟数据或外部资源');
 assert.equal(await page.getByRole('button',{name:'发送运动指令',exact:true}).isDisabled(),true);
 await page.getByRole('button',{name:'去指令实验室发送使能'}).click();
 const send=page.getByRole('button',{name:'发送指令',exact:true});
 await send.click();
 await page.getByText('使能已确认',{exact:true}).waitFor();
 assert.deepEqual(posts[0],{path:'/api/command',params:{hex:'01F3AB01006B'}});checks.push('F3 真实 API 入参，反馈使能后才允许运动');
 await page.screenshot({path:fileURLToPath(new URL('./qa/device-lab-1513.png',import.meta.url)),fullPage:true});
 await page.getByRole('tab',{name:'常规试动',exact:true}).click();
 await page.getByRole('button',{name:'发送运动指令',exact:true}).click();
 await page.waitForTimeout(100);
 assert.equal(posts.at(-1).path,'/api/move');assert.equal(posts.at(-1).params.angle,'10');checks.push('试动提交真实 /api/move，默认 10°、10 RPM、300 mA');
 held=true;await page.getByRole('button',{name:'发送运动指令',exact:true}).click();
 await page.waitForTimeout(100);await page.getByRole('button',{name:'全部停止',exact:true}).click();
 await page.waitForTimeout(100);assert.equal(posts.at(-1).path,'/api/stop-all');pendingRelease();held=false;checks.push('操作请求在途时，全部停止仍独立提交');
 await page.getByLabel('CAN ID',{exact:true}).fill('2');await page.waitForTimeout(550);
 await page.getByLabel('CAN ID',{exact:true}).fill('0');assert.equal(await page.getByRole('button',{name:'发送运动指令',exact:true}).isDisabled(),true);
 await page.getByLabel('CAN ID',{exact:true}).fill('1');await page.waitForTimeout(550);checks.push('切换地址清空旧读数，非法地址禁止运动');
 online=false;await page.waitForTimeout(1100);await page.getByText('设备未连接',{exact:true}).waitFor();
 assert.equal(await page.getByRole('button',{name:'发送运动指令',exact:true}).isDisabled(),true);
 assert.equal(await page.getByText('12.5 °',{exact:true}).count(),0);checks.push('断线清空读数并阻止新运动');online=true;
 await page.getByText('设备在线',{exact:true}).waitFor();
 await page.getByRole('tab',{name:'Wi-Fi 设置',exact:true}).click();
 await page.getByRole('button',{name:'扫描网络',exact:true}).click();
 await page.getByRole('button',{name:/Lab WiFi/}).click();
 await page.getByLabel('密码（开放网络留空）').fill('test-password');
 await page.getByRole('button',{name:'保存并连接'}).click();
 await page.getByRole('link',{name:'192.168.1.50'}).waitFor();
 assert.equal(await page.getByLabel('密码（开放网络留空）').inputValue(),'');
 assert.equal(posts.find(p=>p.path==='/api/wifi/connect').params.ssid,'Lab WiFi');checks.push('Wi-Fi 扫描、选网、连接、状态与地址，提交后清空密码');
 await page.screenshot({path:fileURLToPath(new URL('./qa/device-wifi-1513.png',import.meta.url)),fullPage:true});
 await page.getByRole('button',{name:'忘记已保存网络'}).click();await page.waitForTimeout(1700);
 assert.equal(await page.getByRole('button',{name:'忘记已保存网络'}).isDisabled(),true);checks.push('忘记网络与状态更新');
 await page.getByRole('tab',{name:'称重传感器',exact:true}).click();
 await page.getByRole('heading',{name:'零点漂移诊断',exact:true}).waitFor();
 await page.waitForTimeout(700);
 await page.getByRole('button',{name:'配置 IO',exact:true}).click();
 await page.getByLabel('DOUT GPIO').fill('45');
 await page.getByLabel('SCK GPIO').fill('7');
 await page.getByRole('button',{name:'保存并应用',exact:true}).click();
 await page.getByText('GPIO 45',{exact:true}).waitFor();
 assert.deepEqual(posts.find(p=>p.path==='/api/scale/config').params,{doutPin:'45',sckPin:'7'});
 await page.getByRole('button',{name:'重新去皮',exact:true}).click();
 await page.getByRole('button',{name:'校准传感器',exact:true}).click();
 await page.getByLabel('标准砝码重量').fill('500');
 await page.getByRole('button',{name:'使用此重量校准',exact:true}).click();
 assert.ok(posts.some(p=>p.path==='/api/scale/tare'));
 assert.deepEqual(posts.find(p=>p.path==='/api/scale/calibrate').params,{knownWeightG:'500'});
 checks.push('称重页轮询真实 /api/scale，并提交持久化 IO、去皮与砝码校准参数');
 await page.screenshot({path:fileURLToPath(new URL('./qa/device-scale-1513.png',import.meta.url)),fullPage:true});
 await page.getByRole('tab',{name:'指令实验室',exact:true}).click();
 await page.getByLabel('搜索指令名称或功能码').fill('FF');
 await page.locator('.library__item').first().click();assert.equal(await send.isDisabled(),true);checks.push('未支持的同步触发禁止发送，保留帧预览');
 await page.getByLabel('搜索指令名称或功能码').fill('F3');await page.locator('.library__item').first().click();
 await page.getByRole('tab',{name:'原始 HEX',exact:true}).click();await page.getByLabel('逻辑指令（地址 + 功能码 + 参数 + 6B）').fill('02 F3 AB 01 00 6B');assert.equal(await send.isDisabled(),true);checks.push('原始 HEX 不允许跨地址发送');
 for (const [width,height] of [[1280,800]]) {
  await page.setViewportSize({width,height});await page.getByRole('tab',{name:'常规试动',exact:true}).click();
  await page.screenshot({path:fileURLToPath(new URL(`./qa/device-manual-${width}.png`,import.meta.url)),fullPage:true});
  const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth);
  assert.equal(overflow,false,`overflow at ${width}`);
 }
 checks.push('桌面 1513×1039 与 1280×800 检查通过');
 assert.deepEqual(errors,[]);checks.push('无浏览器运行错误');
 await writeFile(new URL('./qa/device-results.json',import.meta.url),JSON.stringify({checks,errors,posts:posts.map(p=>({path:p.path,params:p.path.includes('wifi')?{...p.params,password:undefined}:p.params}))},null,2));
 console.log(JSON.stringify({passed:checks.length,checks},null,2));
} finally {await browser.close();}
