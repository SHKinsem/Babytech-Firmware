// Real built device page, mocked board only. Start a localhost static server
// for dist/device first. PLAYWRIGHT_MODULE / CHROME_PATH are machine-specific.
import {createRequire} from 'node:module';
import assert from 'node:assert/strict';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
const require=createRequire(import.meta.url);
const {chromium}=require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const base=process.env.QA_BASE_URL || 'http://127.0.0.1:4173';
assert.ok(['localhost','127.0.0.1'].includes(new URL(base).hostname),'QA may only load a local fixture');
const browser=await chromium.launch({headless:true,...(process.env.CHROME_PATH?{executablePath:process.env.CHROME_PATH}:{})});
const hash=text=>{let h=2166136261;for(const b of new TextEncoder().encode(text))h=Math.imul(h^b,16777619)>>>0;return h;};
try {
  const page=await browser.newPage();const errors=[],posts=[];
  page.on('pageerror',e=>errors.push(e.message));page.on('console',m=>{if(m.type()==='error')errors.push(m.text());});
  const source='move 1 90\nmove 2 -90';
  const event={sequence:1,runId:7,iteration:2,line:2,motor:2,function:205,sentAt:100,responseAt:130,code:226,confirmation:'driver_rejected'};
  let queue={state:'done',runId:7,programHash:hash(source),step:2,total:2,iteration:2,repeat:2,line:2,action:'move',message:'done',raw:false,alert:event,diagnostics:[event]};
  let budget={queriesPerSecond:10,gapMs:100,timeoutMs:500,cooldownMs:500,maxInflight:2,txFrames:10,rxFrames:8,driverDrops:null};
  let settings={configured:true,progressTolerance:.2,timeToleranceMs:50,feedbackTimeoutMs:5000,prepareTimeoutMs:10000,stopTimeoutMs:2000,responseBudgetMs:20,completionTenths:2,cacheIsolationReady:false};
  await page.route('**/api/**',async route=>{
    const url=new URL(route.request().url()),path=url.pathname;
    const json=(x,status=200)=>route.fulfill({status,contentType:'application/json',body:JSON.stringify(x)});
    if(route.request().method()==='POST') {
      const values=Object.fromEntries(new URLSearchParams(route.request().postData()));posts.push({path,values});
      if(path==='/api/query-budget'){budget={...budget,...values};return json(budget);}
      if(path==='/api/sync-settings'){settings={...settings,...values};return json(settings);}
      if(path==='/api/sync-isolation'){settings.cacheIsolationReady=!values.revoke;return json(settings);}
      return json({message:'unexpected_mutation'},400);
    }
    if(path==='/api/queue')return json(queue);
    if(path==='/api/query-budget')return json(budget);
    if(path==='/api/sync-settings')return json(settings);
    if(path==='/api/status')return json({id:1,canReady:true,online:true,enabled:true,state:'idle',positionDeg:0,speedRpm:0,currentMa:0,driverEnabled:true,fault:'none',homeOutcome:'none'});
    if(path==='/api/limits')return json({maxSpeedRpm:120,maxAccelRpmS:240,maxCurrentMa:5000,maxAngleDeg:3600,maxMoveSeconds:60,experimentSeconds:5});
    if(path==='/api/motor-distance')return json({id:Number(url.searchParams.get('id')),rotationDistance:8});
    if(path==='/api/trace')return json({uptimeMs:10000,sequence:0,frames:[]});
    if(path==='/api/logs')return json({bootId:'SYNC-QA',uptimeMs:10000,sequence:0,capacity:48,events:[]});
    if(path==='/api/config-result')return json({sequence:0});return json({});
  });
  await page.goto(base);assert.equal(new URL(page.url()).hostname,new URL(base).hostname);assert.ok(await page.title());
  await page.getByRole('tab',{name:'编排队列',exact:true}).click();
  const editor=page.locator('#queue-program');await editor.fill(source);
  const alert=page.locator('.queue-diagnostics [role="alert"]').first();
  await alert.getByText('驱动拒绝',{exact:true}).waitFor();
  assert.match(await page.locator('.queue-col__head').first().innerText(),/发送结束/);
  await page.getByRole('button',{name:'刷新状态',exact:true}).click();
  await alert.getByRole('button',{name:'定位指令'}).click();
  assert.equal(await editor.evaluate(e=>e.value.slice(e.selectionStart,e.selectionEnd)),'move 2 -90');
  await editor.fill('move 9 90');await alert.getByRole('button',{name:'定位指令'}).click();
  await page.getByText(/当前草稿与该运行的源程序不一致/).waitFor();await editor.fill(source);
  await alert.getByRole('button',{name:'定位指令'}).click();
  for(const [width,height] of [[1513,1039],[1280,800]]) {
    await page.setViewportSize({width,height});
    const a=await alert.boundingBox(),start=await page.getByRole('button',{name:'开始执行',exact:true}).boundingBox();
    const trace=await page.locator('.device-trace').boundingBox();
    assert.ok(a.y>=0 && a.y+a.height<height);assert.ok(start.y+start.height<=trace.y);
    const stop=await page.getByRole('button',{name:'全部停止',exact:true}).boundingBox(),tabs=await page.getByRole('tablist').boundingBox();
    assert.ok(stop.y+stop.height<=tabs.y);assert.equal(await page.locator('vite-error-overlay').count(),0);
    await page.screenshot({path:join(tmpdir(),`babytech-sync-${width}.png`)});
  }
  await page.getByText('查看发送与应答详情（1 条）',{exact:true}).click();
  assert.match(await page.locator('.queue-diagnostics table').innerText(),/0xCD/);
  await page.getByText('查看发送与应答详情（1 条）',{exact:true}).click();
  assert.equal(posts.length,0,'opening, reading, editing and diagnostics must never mutate the board');
  await page.getByRole('combobox',{name:'插入指令'}).selectOption('helix');
  const helix={'旋转电机地址':'1','直线电机地址':'2','瓶盖圈数':'3','导程':'2','传动比':'1','旋转方向':'1','直线方向':'-1','各轴转速上限':'1','各轴加速度上限':'60','各轴减速度上限':'60','各轴电流上限':'800','轴向允许偏差':'0.1'};
  for(const [label,value] of Object.entries(helix))await page.getByRole('textbox',{name:`helix ${label}`,exact:true}).fill(value);
  await page.getByRole('button',{name:'插入到程序末尾',exact:true}).click();
  assert.match(await editor.inputValue(),/helix 1 2 3 2 1 1 -1 1 60 60 800 0.1/);assert.equal(posts.length,0);
  await page.getByText('查询预算与同步配置',{exact:true}).click();
  await page.getByRole('textbox',{name:'总查询上限 / 秒',exact:true}).fill('8');
  await page.getByRole('button',{name:'保存全局查询预算'}).click();
  await page.getByText('板端已确认保存；未发送运动指令。',{exact:true}).waitFor();
  assert.equal(posts.length,1);assert.equal(posts[0].values.queriesPerSecond,'8');
  await page.getByText(/启动许可（最后读取）/).click();
  assert.equal(await page.getByRole('button',{name:'记录本次人工隔离确认'}).isDisabled(),true);
  for(const checkbox of await page.locator('.sync-settings input[type="checkbox"]').all())await checkbox.check();
  await page.getByRole('button',{name:'记录本次人工隔离确认'}).click();
  await page.getByText(/启动许可（最后读取）：人工已确认隔离/).waitFor();assert.equal(posts.length,2);
  queue={...queue,state:'cancelled',active:true,sync:{phase:'stop_requested',error:'sync_feedback_lost',members:[{id:1,line:2,stopSent:true,stopped:false}]}};
  await page.getByRole('button',{name:'刷新状态',exact:true}).click();
  await page.getByText(/同步异常：/).waitFor();assert.equal(await page.getByRole('button',{name:'开始执行',exact:true}).isDisabled(),true);
  assert.match(await page.getByLabel('同步成员状态').innerText(),/静止未确认/);
  queue={...queue,state:'done',active:false,motionComplete:true,message:'sync_motion_complete',alert:null,sync:{phase:'complete',members:[]}};
  await page.getByRole('button',{name:'刷新状态',exact:true}).click();
  await page.locator('.queue-col__head').first().getByText(/运动完成/).waitFor();
  assert.deepEqual(errors,[]);assert.equal(posts.length,2);
  console.log('PASS built device UI: two desktop viewports, sticky errors, safe source matching, helix builder, explicit budget/isolation writes, stop-pending lock, honest completion, no automatic POSTs or console errors');
} finally {await browser.close();}
