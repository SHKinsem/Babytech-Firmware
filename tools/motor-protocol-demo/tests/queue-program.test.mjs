// Pure tests for the queue DSL, the queue status payload and the command gate.
// No React, no network, no hardware: node --test runs this file directly.

import assert from 'node:assert/strict';
import test from 'node:test';
import {
  QUEUE_LIMITS,
  SAMPLE_PROGRAM,
  actionAngleDegrees,
  buildActionLine,
  builderDefaults,
  checkRepeat,
  describeAction,
  knownEnabledIds,
  parseProgram,
  previewAction,
  validateProgram,
} from '../src/queue-program.js';
import { DEFAULT_LIMITS } from '../src/device-limits.js';
import {
  homeStatusText,
  isMotionOpcode,
  queueConflictReason,
  queueProgressText,
  readMotorDistance,
  readQueueStatus,
  supportReason,
} from '../src/device-api.js';

const errorsOf = (text, options) => validateProgram(text, options).errors.map((entry) => entry.message);
const linesOf = (text, options) => validateProgram(text, options).errors.map((entry) => entry.line);
const enabledIn = (text) => knownEnabledIds(parseProgram(text).actions);

test('sync groups validate whole structure and preserve ordinary move semantics',()=>{
  assert.equal(validateProgram('sync begin\nmove 1 360\nmove 2 -180\nsync end').ok,true);
  const fast=validateProgram('sync begin trigger\nmove 1 -1080 deg 100 300 300 800\nmove 2 5400 deg 500 1500 1500 500\nsync end');
  assert.equal(fast.ok,true);
  assert.match(fast.preview[0].summary,/不验证运行中 2% 进度/);
  for(const program of ['sync begin\nmove 1 90','sync end','sync begin\nmove 1 90\nsync end',
    'sync begin\nmove 1 90\nmove 1 90\nsync end','sync begin\nmove 1 90 await\nmove 2 90\nsync end',
    'sync begin\nsync begin\nmove 1 90\nmove 2 90\nsync end\nsync end',
    'sync begin\nhex 01 CD 6B\nmove 2 90\nsync end']) assert.equal(validateProgram(program).ok,false,program);
  assert.equal(buildActionLine('sync',{boundary:'end'}).line,'sync end');
  assert.equal(buildActionLine('sync',{boundary:'begin trigger'}).line,'sync begin trigger');
});
test('helix requires explicit geometry and limits, counts expanded steps, and uses board linear conversion',()=>{
  const line='helix 1 2 3 2 1 1 -1 1 60 60 800 0.1';
  const result=validateProgram(line,{distances:{2:8}});
  assert.equal(result.ok,true);assert.equal(result.stats.actions,4);assert.deepEqual(result.stats.usedIds,[1,2]);
  assert.match(result.preview[0].summary,/6 mm/);
  assert.equal(validateProgram(line,{distances:{2:null}}).ok,false);
  assert.equal(buildActionLine('helix',builderDefaults('helix')).ok,false);
  assert.equal(validateProgram(Array(17).fill(line).join('\n')).ok,false);
  for(const bad of [line.replace('1 2 3','1 1 3'),line.replace('1 -1','0 -1'),line.replace('3 2 1','3 0 1'),line+' extra'])
    assert.equal(validateProgram(bad).ok,false,bad);
});
test('sync stop tracking remains active after queue cancellation; ordinary done never claims arrival',()=>{
  const stopped=readQueueStatus({state:'cancelled',active:true,sync:{phase:'stop_requested'}}).status;
  assert.equal(stopped.active,true);assert.match(queueProgressText(stopped),/待确认/);
  assert.match(queueProgressText(readQueueStatus({state:'done',motionComplete:true}).status),/运动完成/);
  assert.match(queueProgressText(readQueueStatus({state:'done'}).status),/发送结束/);
});

test('sample program is short, valid and needs explicit enables', () => {
  const result = validateProgram(SAMPLE_PROGRAM, { distances: { 1: 40 } });
  assert.deepEqual(result.errors, []);
  assert.equal(result.actions.length, 8);
  assert.deepEqual(result.stats.usedIds, [1, 2, 3]);
  assert.deepEqual(result.actions.map((action) => action.verb), ['enable', 'move', 'wait', 'enable', 'home', 'enable', 'torque', 'torque']);

  const move = result.actions.find((action) => action.verb === 'move');
  assert.deepEqual(move, { line: 4, verb: 'move', awaitCompletion: true, id: 1, value: 90, unit: 'deg', rpm: 30, accel: 60, decel: 60, current: 800 });
  assert.deepEqual(result.actions.find((action) => action.verb === 'torque'), { line: 9, verb: 'torque', id: 3, currentMa: 800, durationMs: 1500, maxRpm: 30, rampMaS: 1000 });
  assert.equal(result.actions.find((action) => action.verb === 'home').mode, 0);

  // Every motor that moves is enabled first, on its own line.
  for (const action of result.actions.filter((entry) => ['move', 'home', 'torque'].includes(entry.verb))) {
    assert.ok(
      result.actions.some((entry) => entry.verb === 'enable' && entry.id === action.id && entry.line < action.line),
      `motor ${action.id} must be enabled before line ${action.line}`,
    );
  }
  // No stop/disable padding: a timed torque stops itself and stop keeps enable.
  assert.equal(result.actions.filter((entry) => ['stop', 'disable'].includes(entry.verb)).length, 0);
  assert.deepEqual([...knownEnabledIds(result.actions)].sort(), [1, 2, 3]);
  // The optional disable line is explained in the help instead.
  assert.match(buildActionLine('disable', builderDefaults('disable')).line, /^disable 1$/);

  // Readable preview with source line numbers, and never a torque value in Nm.
  assert.equal(result.preview.length, 8);
  assert.ok(result.preview.every((row) => Number.isInteger(row.line) && row.summary.length > 0));
  const torquePreview = result.preview.find((row) => row.line === 9);
  assert.doesNotMatch(`${torquePreview.summary} ${torquePreview.detail}`, /\d\s*Nm/);
  assert.match(torquePreview.summary, /800 mA · 1500 ms/);
});

test('units, defaults and mm conversion follow the contract', () => {
  assert.equal(parseProgram('move 1 90').actions[0].unit, 'deg');
  assert.equal(parseProgram('move 1 90 12').actions[0].rpm, 12);
  assert.equal(parseProgram('MOVE 1 90 DEG 12 60 60 800').actions[0].rpm, 12);
  assert.equal(parseProgram('move 1 2 rev 30').actions[0].unit, 'rev');
  assert.equal(actionAngleDegrees(parseProgram('move 1 2 rev').actions[0], {}).deg, 720);
  assert.equal(actionAngleDegrees(parseProgram('move 1 90').actions[0], {}).deg, 90);
  // Signed values carry direction; the magnitude check uses |value|.
  assert.equal(parseProgram('move 1 -90').actions[0].value, -90);
  assert.equal(parseProgram('torque 3 -300 1500').actions[0].currentMa, -300);
  assert.equal(parseProgram('velocity 2 -60 2000').actions[0].rpm, -60);
  // Defaults stay visible and are never silently clamped to policy.
  assert.deepEqual(
    { ...parseProgram('torque 1 300 1000').actions[0], line: undefined, verb: undefined },
    { line: undefined, verb: undefined, id: 1, currentMa: 300, durationMs: 1000, maxRpm: 30, rampMaS: 1000 },
  );
  assert.deepEqual(
    { ...parseProgram('velocity 1 60 1000').actions[0], line: undefined, verb: undefined },
    { line: undefined, verb: undefined, id: 1, rpm: 60, durationMs: 1000, accel: 60, current: 800 },
  );
  assert.equal(parseProgram('home 2').actions[0].mode, 0);
});

test('the enable state is followed in order, not accumulated', () => {
  assert.deepEqual([...enabledIn('enable 1\nmove 1 90')], [1]);
  // enable then disable leaves the motor off again: an earlier enable line is
  // not a licence to skip the enable the builder would otherwise insert.
  assert.deepEqual([...enabledIn('enable 1\ndisable 1\nmove 1 90')], []);
  assert.deepEqual([...enabledIn('enable 1\nenable 2\ndisable 1')], [2]);
  assert.deepEqual([...enabledIn('disable 1\nenable 1')], [1]);
  // stop keeps enable (the board preserves it), the timed moves do too.
  assert.deepEqual([...enabledIn('enable 1\nmove 1 90\nstop 1\ntorque 1 800 1500')], [1]);
  // A raw frame can carry anything, including a disable, so nothing stays known.
  assert.deepEqual([...enabledIn('enable 1\nenable 2\nhex 01 F3 AB 00 00 6B')], []);
  assert.deepEqual([...enabledIn('enable 1\ncan ext 100 F3 AB 00 00')], []);
  // ... and a program that never enables anything knows nothing.
  assert.deepEqual([...enabledIn('move 1 90\nhome 2')], []);
  assert.deepEqual([...knownEnabledIds([])], []);
});

test('mm requires the confirmed per-ID rotation distance, never a guessed one', () => {
  const known = validateProgram('move 1 90 mm', { distances: { 1: 40 } });
  assert.deepEqual(known.errors, []);
  assert.match(known.preview[0].detail, /旋转距离 40 mm\/rev → 810°/);
  assert.equal(actionAngleDegrees(known.actions[0], { 1: 40 }).deg, 810);

  // Confirmed "no distance saved": the board cannot convert, so this blocks.
  const missing = validateProgram('move 1 90 mm', { distances: { 1: null } });
  assert.equal(missing.ok, false);
  assert.match(missing.errors[0].message, /旋转距离为 0（未配置）/);
  assert.match(missing.warnings[0].message, /尚未保存 mm\/rev/);

  // Failed read: unknown stays unknown — a warning, never an assumed default.
  const unknown = validateProgram('move 1 90 mm', { distances: {} });
  assert.equal(unknown.ok, true);
  assert.match(unknown.warnings[0].message, /不推测换算结果/);
  assert.match(unknown.preview[0].detail, /旋转距离未读取/);

  // Other IDs are independent.
  assert.equal(validateProgram('move 2 90 mm', { distances: { 1: 40 } }).ok, true);
  assert.equal(validateProgram('move 2 90 mm', { distances: { 1: 40 } }).warnings.length, 1);
});

test('zero-rounding is reported before the board rejects the frame', () => {
  const tiny = validateProgram('move 1 0.1 mm', { distances: { 1: 1000000 } });
  assert.equal(tiny.ok, true);
  assert.match(tiny.warnings[0].message, /不足 1 个 0\.1° 计数/);
  // Below the firmware's minimum angle is a framing error, not a policy error.
  assert.match(errorsOf('move 1 0.01 deg')[0], /不能小于 0\.1/);
});

test('unknown verbs and malformed numbers point at the source line', () => {
  const result = validateProgram('# comment\n\nenable 1\nfly 2\nmove 1 90\n');
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].line, 4);
  assert.match(result.errors[0].message, /未知指令「fly」/);
  assert.match(result.errors[0].message, /enable/);
  assert.equal(result.actions.length, 2);

  for (const bad of ['1e3', '0x10', '+5', '.5', '1.', 'nan', 'Infinity', '--1', '1,5']) {
    const errors = validateProgram(`move 1 ${bad}`).errors;
    assert.equal(errors.length, 1, `${bad} must be rejected`);
    assert.equal(errors[0].line, 1);
    assert.match(errors[0].message, /只接受十进制数字/);
  }
  assert.deepEqual(validateProgram('move 1 0.5 deg').errors, []);
});

test('ranges and overflow are rejected instead of truncated', () => {
  const cases = [
    ['move 0 90', /电机地址/],
    ['move 256 90', /电机地址/],
    ['move 1 90 deg 3001', /转速/],
    ['move 1 90 deg 30 65536', /加速度/],
    ['move 1 90 deg 30 60 -1', /减速度/],
    ['move 1 90 deg 30 60 60 5001', /电流上限/],
    ['move 1 90 deg 30 60 60 -1', /电流上限/],
    ['move 1 90 deg 30 60 60 800 7', /最多 4 个可选参数/],
    ['home 2 6', /回零模式/],
    ['home 2 0 1', /用法：home ID \[MODE\]/],
    ['torque 1 300 0', /持续时间/],
    ['torque 1 300 3600001', /持续时间/],
    ['torque 1 6000 100', /力矩电流/],
    ['torque 1 300 100 3001', /限速/],
    ['velocity 1 3001 100', /转速/],
    ['velocity 1 0 100', /转速/],
    ['wait -1', /等待时间/],
    ['wait 3600001', /等待时间/],
    ['wait 1.5', /整数/],
  ];
  for (const [line, pattern] of cases) {
    const errors = errorsOf(line);
    assert.equal(errors.length, 1, `${line} must produce exactly one error`);
    assert.match(errors[0], pattern, line);
    assert.equal(linesOf(line)[0], 1);
  }
  // Boundary values stay accepted: the page does not apply board policy itself.
  assert.deepEqual(validateProgram('move 255 360000 deg 3000 65535 65535 5000').errors, []);
  assert.deepEqual(validateProgram('wait 0\nwait 3600000\ntorque 1 5000 3600000 3000 65535').errors, []);
});

test('raw logical hex keeps every byte and checks its bounds', () => {
  assert.deepEqual(parseProgram('hex 01 9A 00 00 6B').actions[0].bytes, [0x01, 0x9a, 0x00, 0x00, 0x6b]);
  assert.deepEqual(parseProgram('HEX 01 9a 00 00 6b').actions[0].bytes, [0x01, 0x9a, 0x00, 0x00, 0x6b]);
  assert.match(errorsOf('hex 01 9A')[0], new RegExp(`${QUEUE_LIMITS.hexMinBytes}\\.\\.${QUEUE_LIMITS.hexMaxBytes} 字节`));
  assert.match(errorsOf(`hex ${Array.from({ length: 31 }, () => '01').join(' ')}`)[0], /31 字节/);
  assert.deepEqual(validateProgram(`hex ${Array.from({ length: 30 }, () => '01').join(' ')}`).errors, []);
  assert.match(errorsOf('hex 1 9A 00 00 6B')[0], /两位十六进制字符/);
  assert.match(errorsOf('hex 01 9A 0G 00 6B')[0], /两位十六进制字符/);
  // Missing address/checksum framing is a warning: the raw path is deliberate.
  const raw = validateProgram('hex 00 9A 00 00 01');
  assert.deepEqual(raw.errors, []);
  assert.match(raw.warnings[0].message, /首字节应为地址、末字节应为固定校验 6B/);
  assert.equal(raw.preview[0].raw, true);
  assert.match(raw.preview[0].detail, /无运动监督/);
});

test('raw CAN frames bound the ID form, length and DLC', () => {
  const frame = parseProgram('can ext 100 9A 00 00').actions[0];
  assert.deepEqual(frame, { line: 1, verb: 'can', extended: true, canId: 0x100, data: [0x9a, 0x00, 0x00] });
  assert.deepEqual(parseProgram('can ext 0x1FFFFFFF').actions[0].data, []);
  assert.equal(parseProgram('can std 7FF 01').actions[0].extended, false);
  assert.equal(parseProgram('CAN EXT 100 00').actions[0].extended, true);
  const cases = [
    ['can ext 20000000 00', /扩展帧 ID/],
    ['can std 800 00', /标准帧 ID/],
    ['can ext 100 01 02 03 04 05 06 07 08 09', /最多 8 字节/],
    ['can 100 00', /必须是 ext（扩展帧）或 std（标准帧）/],
    ['can ext', /缺少 ID/],
    ['can ext zz 00', /十六进制字符/],
    ['can ext 100 1', /两位十六进制字符/],
  ];
  for (const [line, pattern] of cases) {
    const errors = errorsOf(line);
    assert.equal(errors.length, 1, line);
    assert.match(errors[0], pattern, line);
  }
  const raw = validateProgram('can ext 100 9A 00 00');
  assert.equal(raw.preview[0].raw, true);
  assert.match(raw.preview[0].summary, /ID 0x00000100/);
  assert.match(raw.preview[0].detail, /相邻帧之间至少 2 ms/);
});

test('program-level limits match the board contract', () => {
  const long = Array.from({ length: 65 }, (_, index) => `wait ${index}`).join('\n');
  assert.match(errorsOf(long)[0], /超过板端上限 64 个/);
  assert.deepEqual(validateProgram(Array.from({ length: 64 }, () => 'wait 1').join('\n')).errors, []);
  const oversized = `#${'x'.repeat(QUEUE_LIMITS.maxTextBytes)}`;
  assert.match(errorsOf(oversized)[0], /超过板端上限 8192 字节/);
  assert.match(errorsOf('# 只有注释\n\n   \n')[0], /没有任何动作/);
  assert.equal(parseProgram('# 注释\nwait 1 # 行尾注释\n').actions.length, 1);
});

test('repeat count is an integer 1..1000', () => {
  assert.deepEqual(checkRepeat('1'), { ok: true, value: 1 });
  assert.deepEqual(checkRepeat(' 1000 '), { ok: true, value: 1000 });
  for (const bad of ['0', '1001', '1.5', '-1', '', 'abc', '1e2']) {
    assert.equal(checkRepeat(bad).ok, false, bad);
  }
});

test('the action builder produces lines the parser accepts', () => {
  for (const verb of ['enable', 'disable', 'move', 'home', 'torque', 'velocity', 'stop', 'wait', 'hex', 'can']) {
    const built = buildActionLine(verb, builderDefaults(verb));
    assert.equal(built.ok, true, `${verb}: ${built.error}`);
    const [action] = parseProgram(built.line).actions;
    assert.equal(action.verb, verb);
    assert.deepEqual(validateProgram(built.line, { distances: { 1: 40 } }).errors, []);
  }
  assert.equal(buildActionLine('move', { ...builderDefaults('move'), id: 'abc' }).ok, false);
  assert.equal(buildActionLine('nope', {}).ok, false);
  // A user-visible hint is available for every verb.
  for (const verb of ['move', 'home', 'torque', 'velocity', 'hex', 'can']) {
    const built = buildActionLine(verb, builderDefaults(verb));
    assert.ok(describeAction(built.action, { distances: { 1: 40 } }).length > 8);
  }
});

test('preview wording keeps the honest caveats', () => {
  assert.match(previewAction(parseProgram('move 1 90').actions[0], {}).summary, /相对运动 \+90 deg/);
  assert.match(previewAction(parseProgram('home 2 2').actions[0], {}).summary, /无限位碰撞/);
  const torque = previewAction(parseProgram('torque 1 -300 1000').actions[0], {});
  assert.match(torque.summary, /-300 mA/);
  assert.doesNotMatch(`${torque.summary} ${torque.detail}`, /\d\s*Nm/);
  assert.match(previewAction(parseProgram('disable 1').actions[0], {}).summary, /不等应答/);
  assert.match(previewAction(parseProgram('wait 200').actions[0], {}).summary, /非阻塞/);
});

test('queue status payloads are read strictly', () => {
  const good = readQueueStatus({ state: 'running', runId: 7, step: 3, total: 9, iteration: 2, repeat: 3, line: 12, action: 'move 1 90', message: '', raw: false });
  assert.equal(good.ok, true);
  assert.equal(good.status.step, 3);
  assert.match(queueProgressText(good.status), /运行中 · 第 3\/9 步 · 第 2\/3 轮 · 源程序第 12 行/);
  assert.match(queueProgressText(readQueueStatus({ state: 'idle' }).status), /尚未开始第一步/);
  // A board that answered something else is not "idle".
  for (const payload of [{}, null, [], { state: 'done!' }, 'running']) {
    assert.equal(readQueueStatus(payload).ok, false, JSON.stringify(payload));
  }
});

test('homing status comes from the real controller fields', () => {
  // Fixture shape of MotorControl::statusJson (homeOutcome/homeId/homeMode/
  // homeOrg/homeRunning/homeFailed).
  const running = { id: 2, homeOutcome: 'running', homeId: 2, homeMode: 2, homeOrg: 0x04, homeRunning: true, homeFailed: false, homeActive: true };
  const runningText = homeStatusText(running);
  assert.match(runningText, /回零进行中/);
  assert.match(runningText, /模式 2/);
  assert.match(runningText, /3B 0x04 · 正在回零/);

  // The outcome belongs to homeId, so another motor's conclusion is never shown
  // as this one's.
  assert.equal(homeStatusText({ ...running, id: 1 }), null);
  assert.equal(homeStatusText({ ...running, homeId: 0 }), null);
  assert.equal(homeStatusText({ id: 2, homeOutcome: 'none', homeId: 2, homeOrg: null, homeRunning: null, homeFailed: null }), null);
  assert.equal(homeStatusText({ id: 2, homeOutcome: 'done' }), null);
  assert.equal(homeStatusText(null), null);

  const done = { id: 1, homeOutcome: 'done', homeId: 1, homeMode: 0, homeOrg: 0x00, homeRunning: false, homeFailed: false };
  assert.match(homeStatusText(done), /回零已完成/);
  assert.match(homeStatusText(done), /3B 0x00 · 无标志置位/);

  // no_motion (manual 12/22) is its own outcome: the attempt ended, the motor
  // did not move, and it is not a completion.
  const noMotion = homeStatusText({ id: 3, homeOutcome: 'no_motion', homeId: 3, homeMode: 2, homeOrg: null, homeRunning: false, homeFailed: false });
  assert.match(noMotion, /电机未动/);
  assert.match(noMotion, /12\/22/);
  assert.match(noMotion, /没有完成回零/);
  assert.doesNotMatch(noMotion, /已完成回零/);

  assert.match(homeStatusText({ id: 1, homeOutcome: 'cancelled', homeId: 1 }), /回零已取消/);
  assert.match(homeStatusText({ id: 1, homeOutcome: 'failed', homeId: 1, homeOrg: 0x08, homeFailed: true }), /回零失败.*回零失败/);
  // homeOrg is the raw 0x3B byte: reserved bits are never given a meaning.
  assert.match(homeStatusText({ id: 1, homeOutcome: 'running', homeId: 1, homeOrg: 0x14, homeRunning: true }), /3B 0x14 · 正在回零 \/ 过热保护/);
  assert.match(homeStatusText({ id: 1, homeOutcome: 'running', homeId: 1, homeOrg: 0x80, homeRunning: true }), /3B 0x80 · 无标志置位/);
  // A 3B byte that contradicts the verdict is reported, not smoothed over.
  assert.match(homeStatusText({ id: 1, homeOutcome: 'done', homeId: 1, homeOrg: 0x04, homeRunning: true, homeFailed: false }), /不一致.*按未完成看待/);
  assert.match(homeStatusText({ id: 1, homeOutcome: 'done', homeId: 1, homeOrg: 0x08, homeRunning: false, homeFailed: true }), /不一致.*按失败看待/);
});

test('rotation distance payloads are adopted only when they are trustworthy', () => {
  assert.deepEqual(readMotorDistance({ id: 1, rotationDistance: 40 }, 1), { ok: true, id: 1, value: 40 });
  assert.deepEqual(readMotorDistance({ id: 3, rotationDistance: 0 }, 3), { ok: true, id: 3, value: null });
  assert.deepEqual(readMotorDistance({ id: 3, rotationDistance: null }, 3), { ok: true, id: 3, value: null });
  assert.equal(readMotorDistance({ id: 1, rotationDistance: 0.000001 }, 1).ok, true);
  assert.equal(readMotorDistance({ id: 1, rotationDistance: 1000000 }, 1).ok, true);

  // A response for another address is never applied to the selected one.
  const mismatch = readMotorDistance({ id: 2, rotationDistance: 40 }, 1);
  assert.equal(mismatch.ok, false);
  assert.equal(mismatch.mismatch, true);
  assert.match(mismatch.error, /不一致/);

  // Bounds are checked, not just "positive".
  for (const bad of [0.0000001, 1000001, -5, 'abc', NaN, Infinity, true]) {
    assert.equal(readMotorDistance({ id: 1, rotationDistance: bad }, 1).ok, false, String(bad));
  }
  assert.match(readMotorDistance({ id: 1 }, 1).error, /缺少 rotationDistance/);
  assert.equal(readMotorDistance({ id: 0, rotationDistance: 40 }).ok, false, 'address 0 is not a motor');
  assert.equal(readMotorDistance({ id: 999, rotationDistance: 40 }).ok, false);
  assert.equal(readMotorDistance(null).ok, false);
  assert.equal(readMotorDistance('nope').ok, false);
});

test('a running queue locks manual mutations but keeps reads, stop, disable and interrupt', () => {
  const move = [1, 0xcd, 0, 0, 0, 0, 60, 60, 0, 0x0a, 0, 0, 0, 2, 0, 0x64, 0, 0x6b];
  assert.match(queueConflictReason(move, { running: true }), /队列正在运行/);
  assert.match(queueConflictReason(move, { unknown: true }), /提交结果未知/);
  assert.equal(queueConflictReason(move, {}), null);
  assert.equal(queueConflictReason(move), null);

  // 0xF3 carries both directions: disable is allowed, a manual enable is not —
  // it would collide with the enable state the running program owns.
  const disable = [1, 0xf3, 0xab, 0, 0, 0x6b];
  const enable = [1, 0xf3, 0xab, 1, 0, 0x6b];
  assert.equal(queueConflictReason(disable, { running: true }), null);
  assert.equal(queueConflictReason(disable, { unknown: true }), null);
  assert.match(queueConflictReason(enable, { running: true }), /手动使能会被板端拒绝/);
  assert.match(queueConflictReason(enable, { unknown: true }), /手动使能会被板端拒绝/);
  assert.doesNotMatch(queueConflictReason(enable, { running: true }), /^板端队列正在运行：普通指令/);

  // Reads, stop and the homing interrupt stay available in both lock states.
  for (const opcode of [0x36, 0x3a, 0x3b, 0x3c]) {
    assert.equal(queueConflictReason([1, opcode, 0x6b], { running: true }), null, `read ${opcode.toString(16)}`);
  }
  for (const frame of [[1, 0xfe, 0x98, 0, 0x6b], [1, 0x9c, 0x48, 0x6b]]) {
    assert.equal(queueConflictReason(frame, { running: true }), null);
    assert.equal(queueConflictReason(frame, { unknown: true }), null);
  }
  // A frame without a function code is locked rather than waved through.
  assert.match(queueConflictReason([], { running: true }), /队列正在运行/);
});

test('homing is supervised now: 9A is a gated motion, not a preview-only frame', () => {
  assert.equal(isMotionOpcode(0x9a), true);
  const home = [1, 0x9a, 0x02, 0x00, 0x6b];
  assert.equal(supportReason(home, DEFAULT_LIMITS), null);
  assert.doesNotMatch(String(supportReason(home, DEFAULT_LIMITS)), /仅预览/);
  assert.match(supportReason([1, 0x9a, 0x06, 0x00, 0x6b], DEFAULT_LIMITS), /回零模式只能是 0\.\.5/);
  assert.match(supportReason([1, 0x9a, 0x00, 0x01, 0x6b], DEFAULT_LIMITS), /同步标志必须为 0/);
  assert.match(supportReason([1, 0x9a, 0x00, 0x6b], DEFAULT_LIMITS), /5 字节/);
  // The remaining preview-only paths are unchanged.
  assert.match(supportReason([1, 0xfd, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x6b], DEFAULT_LIMITS), /仅预览/);
});

test('4C homing parameters follow the configured speed and current policy', () => {
  const frame = ({ vel = 30, timeoutMs = 10000, stallMa = 800, stallVel = 300, powerOn = 0 } = {}) => [
    1, 0x4c, 0xae, 1, 0, 0,
    (vel >> 8) & 0xff, vel & 0xff,
    (timeoutMs >>> 24) & 0xff, (timeoutMs >>> 16) & 0xff, (timeoutMs >>> 8) & 0xff, timeoutMs & 0xff,
    (stallVel >> 8) & 0xff, stallVel & 0xff,
    (stallMa >> 8) & 0xff, stallMa & 0xff,
    0, 60, powerOn, 0x6b,
  ];
  assert.equal(supportReason(frame(), DEFAULT_LIMITS), null);
  // The default 300 RPM collision threshold is a threshold, not a commanded
  // speed, so the speed policy applies to the homing velocity only.
  assert.equal(supportReason(frame({ stallVel: 3000 }), DEFAULT_LIMITS), null);
  assert.match(supportReason(frame({ vel: 300 }), DEFAULT_LIMITS), /回零速度 300 RPM 超出板端当前上限 120 RPM/);
  assert.equal(supportReason(frame({ vel: 120 }), DEFAULT_LIMITS), null);
  // Raising the editable policy raises the page's verdict with it.
  assert.equal(supportReason(frame({ vel: 300 }), { ...DEFAULT_LIMITS, maxSpeedRpm: 3000 }), null);
  assert.match(supportReason(frame({ stallMa: 5001 }), DEFAULT_LIMITS), /碰撞检测电流 5001 mA 超出板端当前上限 5000 mA/);
  assert.equal(supportReason(frame({ stallMa: 5000 }), DEFAULT_LIMITS), null);
  // The timeout is a full uint32 on the wire: no arbitrary cap in the page.
  assert.equal(supportReason(frame({ timeoutMs: 0xffffffff }), DEFAULT_LIMITS), null);
  assert.match(supportReason(frame({ powerOn: 1 }), DEFAULT_LIMITS), /不允许配置上电自动回零/);
});

test('await suffix is explicit, strict and shown in previews', () => {
  const parsed = parseProgram('move 1 90\nmove 1 90 deg 30 60 60 800 AWAIT # done\nhome 2 2\nhome 2 await');
  assert.deepEqual(parsed.errors, []);
  assert.deepEqual(parsed.actions.map(a => a.awaitCompletion), [false, true, false, true]);
  assert.match(previewAction(parsed.actions[0]).summary, /发送后继续/);
  assert.match(previewAction(parsed.actions[1]).summary, /等待到位/);
  assert.match(previewAction(parsed.actions[3]).summary, /等待完成/);
  for (const text of ['move 1 await 90', 'home 2 await await', 'stop 1 await', 'wait 2 await', 'hex 01 FE 6B await']) {
    assert.equal(parseProgram(text).errors.length, 1, text);
  }
});
