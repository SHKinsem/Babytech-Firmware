import assert from 'node:assert/strict';
import test from 'node:test';
import {DEFAULT_LIMITS,deviceDefaults,readLimitsPayload,limitsToManualLimits,checkLimitsDraft} from '../src/device-limits.js';
import {supportReason,directPositionBoardNote,isMotionOpcode,isLimitDependentOpcode,MOTION_OPS} from '../src/device-api.js';
import {planManualMove,MANUAL_DEFAULTS} from '../src/simulation.js';
import {encodeCommand,getCommandItem} from '../src/protocol.js';

test('screenshot acceleration 1000 follows confirmed board configuration',()=>{
 const bytes=[2,0xf6,0,3,0xe8,3,0xe8,0,0x6b];
 assert.match(supportReason(bytes,DEFAULT_LIMITS),/加速度.*1000.*240/);
 assert.equal(supportReason(bytes,{...DEFAULT_LIMITS,maxAccelRpmS:2000}),null);
 assert.match(supportReason(bytes,{...DEFAULT_LIMITS,maxAccelRpmS:2000,maxSpeedRpm:50}),/速度.*100.*50/);
});
test('manual planner adopts same limits and duration',()=>{
 const values={...MANUAL_DEFAULTS,accel:'1000',decel:'1000',speed:'300'};
 assert.equal(planManualMove(values).ok,false);
 assert.equal(planManualMove(values,limitsToManualLimits({...DEFAULT_LIMITS,maxAccelRpmS:2000,maxSpeedRpm:500})).ok,true);
 const long={...MANUAL_DEFAULTS,angle:'7200',speed:'10'};
 assert.equal(planManualMove(long).ok,false);
 assert.equal(planManualMove(long,limitsToManualLimits({...DEFAULT_LIMITS,maxAngleDeg:10000,maxMoveSeconds:180})).ok,true);
});
test('complete finite settings required, 0 timer is explicit, no silent clamp',()=>{
 assert.equal(readLimitsPayload({}).ok,false);
 for(const invalid of [{maxSpeedRpm:3000.1},{maxAccelRpmS:65536},{maxCurrentMa:5001},{maxMoveSeconds:0},{experimentSeconds:-1},{experimentSeconds:1.5},{maxAngleDeg:NaN}])
   assert.equal(checkLimitsDraft({...DEFAULT_LIMITS,...invalid}).ok,false);
 assert.equal(checkLimitsDraft({...DEFAULT_LIMITS,experimentSeconds:0}).ok,true);
});

// ---------------------------------------------------------------------------
// Direct position (FB / CB) is supervised by the board now
// ---------------------------------------------------------------------------

// The examples from the manual's own worked case: motor 2, CW, 300 (0.1 RPM),
// 900 (0.1 deg), mode 2, immediate. CB adds 800 mA.
const FB_EXAMPLE=[2,0xfb,0,1,0x2c,0,0,3,0x84,2,0,0x6b];
const CB_EXAMPLE=[2,0xcb,0,1,0x2c,0,0,3,0x84,2,0,3,0x20,0x6b];
const withByte=(frame,index,value)=>{const copy=[...frame];copy[index]=value;return copy;};
const withSpeed=(frame,value)=>[...frame.slice(0,3),(value>>8)&0xff,value&0xff,...frame.slice(5)];
const withAngle=(frame,value)=>[...frame.slice(0,5),(value>>>24)&0xff,(value>>>16)&0xff,(value>>>8)&0xff,value&0xff,...frame.slice(9)];
const withCurrent=(frame,value)=>[...frame.slice(0,11),(value>>8)&0xff,value&0xff,0x6b];
// The same frame with a different function code: used to prove the opcode is
// preserved and that each form keeps its own documented length.
const asOpcode=(frame,opcode)=>withByte(frame,1,opcode);

test('FB and CB are gated motions with the documented layout, not previews',()=>{
 for(const frame of [FB_EXAMPLE,CB_EXAMPLE]){
  assert.equal(supportReason(frame,DEFAULT_LIMITS),null);
  assert.doesNotMatch(String(supportReason(frame,DEFAULT_LIMITS)),/仅预览/);
  // Limits are still required before the page may send a motion command.
  assert.equal(isMotionOpcode(frame[1]),true);
 }
 assert.ok(MOTION_OPS.includes(0xfb)&&MOTION_OPS.includes(0xcb));
 // The remaining preview-only path is unchanged.
 assert.match(supportReason([1,0xfd,0,0,0,0,1,0,0,0,0,0,0,0,0,0x6b],DEFAULT_LIMITS),/仅预览/);
});

test('the form encodes the documented FB and CB frames byte for byte',()=>{
 const item=getCommandItem('passthroughPosition');
 const fb=encodeCommand({item,variantKey:'base',values:{...deviceDefaults(item,'base',DEFAULT_LIMITS),dir:0,vel:300,clk:900,motionMode:2,sync:0},address:2});
 assert.deepEqual(fb.bytes,FB_EXAMPLE);
 const cb=encodeCommand({item,variantKey:'limit',values:{...deviceDefaults(item,'limit',DEFAULT_LIMITS),dir:0,vel:300,clk:900,motionMode:2,sync:0,maxCurrentMa:800},address:2});
 assert.deepEqual(cb.bytes,CB_EXAMPLE);
 // The opcode itself is preserved: FB stays FB, CB stays CB.
 assert.equal(fb.bytes[1],0xfb);assert.equal(cb.bytes[1],0xcb);
});

test('FB/CB layout, sync, direction and motion mode mirror the board gate',()=>{
 // Each form keeps its own documented length: FB is 12 bytes, CB is 14. A
 // CB-declared frame of the FB length is the truncation worth rejecting here.
 assert.match(supportReason(asOpcode(FB_EXAMPLE,0xcb),DEFAULT_LIMITS),/CB 为 14 字节/);
 assert.match(supportReason(CB_EXAMPLE.slice(0,13),DEFAULT_LIMITS),/CB 为 14 字节/);
 assert.match(supportReason(asOpcode(CB_EXAMPLE,0xfb),DEFAULT_LIMITS),/FB 为 12 字节/);
 assert.match(supportReason(FB_EXAMPLE.slice(0,11),DEFAULT_LIMITS),/FB 为 12 字节/);
 assert.match(supportReason(withByte(FB_EXAMPLE,10,1),DEFAULT_LIMITS),/同步标志必须为 0/);
 assert.match(supportReason(withByte(CB_EXAMPLE,10,1),DEFAULT_LIMITS),/同步标志必须为 0/);
 assert.match(supportReason(withByte(FB_EXAMPLE,2,2),DEFAULT_LIMITS),/方向/);
 // All three documented motion modes are usable now.
 for(const mode of [0,1,2]) assert.equal(supportReason(withByte(FB_EXAMPLE,9,mode),DEFAULT_LIMITS),null);
 assert.match(supportReason(withByte(FB_EXAMPLE,9,3),DEFAULT_LIMITS),/运动模式/);
});

test('FB/CB value policy mirrors the board limits',()=>{
 // Speed follows the configured policy, in 0.1 RPM on the wire.
 assert.match(supportReason(withSpeed(FB_EXAMPLE,0x400),{...DEFAULT_LIMITS,maxSpeedRpm:50}),/速度 102\.4 RPM 超出板端当前上限 50 RPM/);
 // Modes 0 and 2 carry a displacement, bounded like the CD travel field.
 assert.match(supportReason(withAngle(FB_EXAMPLE,36010),DEFAULT_LIMITS),/行程 3601 ° 超出板端当前上限 3600 °/);
 assert.match(supportReason(withAngle(FB_EXAMPLE,2000),{...DEFAULT_LIMITS,maxAngleDeg:100}),/行程 200 ° 超出板端当前上限 100 °/);
 assert.equal(supportReason(withAngle(FB_EXAMPLE,36000),DEFAULT_LIMITS),null);
 // Mode 1 is an absolute coordinate: the travel policy is not applied to the
 // coordinate itself (the board measures |target - current| instead), only the
 // int32 the driver can report back.
 assert.equal(supportReason(withByte(withAngle(FB_EXAMPLE,1000000),9,1),DEFAULT_LIMITS),null);
 assert.match(supportReason(withByte(withAngle(FB_EXAMPLE,0x80000000),9,1),DEFAULT_LIMITS),/int32/);
 // CB current: the manual's 0..5000 mA, subject to the configured policy and
 // with no arbitrary 100 mA floor.
 assert.equal(supportReason(withCurrent(CB_EXAMPLE,0),DEFAULT_LIMITS),null);
 assert.equal(supportReason(withCurrent(CB_EXAMPLE,50),{...DEFAULT_LIMITS,maxCurrentMa:500}),null);
 assert.match(supportReason(withCurrent(CB_EXAMPLE,800),{...DEFAULT_LIMITS,maxCurrentMa:500}),/电流上限 800 mA 超出板端当前上限 500 mA/);
 assert.equal(supportReason(CB_EXAMPLE,DEFAULT_LIMITS),null);
 // FB has no current field at all, so no per-command current limit applies.
 assert.equal(supportReason(FB_EXAMPLE,{...DEFAULT_LIMITS,maxCurrentMa:100}),null);
});

test('zero speed is only allowed where the page can prove a zero-travel no-op',()=>{
 const zeroSpeed=withSpeed(FB_EXAMPLE,0);
 // Mode 2: the travel is exactly the angle field, so the page decides it.
 assert.match(supportReason(zeroSpeed,DEFAULT_LIMITS),/零位移/);
 assert.equal(supportReason(withAngle(zeroSpeed,0),DEFAULT_LIMITS),null);
 // Modes 0 and 1 are resolved by the board against fresh feedback and the 0x33
 // target read, so the page does not pre-empt them.
 for(const mode of [0,1]) assert.equal(supportReason(withByte(zeroSpeed,9,mode),DEFAULT_LIMITS),null);
});

test('closed-loop current limit 0x45 needs limits but is not a motion',()=>{
 const frame=(save=0,currentMa=120)=>[1,0x45,0x66,save,(currentMa>>8)&0xff,currentMa&0xff,0x6b];
 // Layout: 7 bytes, auxiliary byte 66, save 00/01, documented 0..5000 mA.
 assert.equal(supportReason(frame(),DEFAULT_LIMITS),null);
 assert.equal(supportReason(frame(1,100),DEFAULT_LIMITS),null);
 assert.match(supportReason([1,0x45,0x66,0,0,120],DEFAULT_LIMITS),/7 字节/);
 assert.match(supportReason([1,0x45,0x33,0,0,120,0x6b],DEFAULT_LIMITS),/辅助码固定为 66/);
 assert.match(supportReason(frame(2,120),DEFAULT_LIMITS),/是否存储只能是 0/);
 assert.match(supportReason(frame(0,5001),DEFAULT_LIMITS),/协议范围 0\.\.5000/);
 // The configured current policy is a ceiling for this write too.
 assert.equal(supportReason(frame(0,100),{...DEFAULT_LIMITS,maxCurrentMa:100}),null);
 assert.match(supportReason(frame(0,120),{...DEFAULT_LIMITS,maxCurrentMa:100}),/闭环最大相电流 120 mA 超出板端当前上限 100 mA/);
 // It is gated on confirmed limits without being claimed as motion (no enable).
 assert.equal(isMotionOpcode(0x45),false);
 assert.equal(isLimitDependentOpcode(0x45),true);
 for(const op of MOTION_OPS)assert.equal(isLimitDependentOpcode(op),true);
 assert.equal(isLimitDependentOpcode(0x46),false);
});

test('coordinate-dependent checks are reported as board-resolved',()=>{
 // Mode 0 names the p70 target position read (0x33) as the baseline, and says
 // explicitly that the p71 setpoint (0x34) is not a substitute.
 const modeZero=withByte(FB_EXAMPLE,9,0);
 assert.match(directPositionBoardNote(modeZero),/0x33/);
 assert.match(directPositionBoardNote(modeZero),/新鲜读值/);
 assert.match(directPositionBoardNote(modeZero),/0x34 实时设定值/);
 // Mode 1 explains that the bounded quantity is the real travel.
 assert.match(directPositionBoardNote(withByte(FB_EXAMPLE,9,1)),/实际行程/);
 // Mode 2 is the current-position baseline, for both forms.
 assert.match(directPositionBoardNote(FB_EXAMPLE),/当前实际位置/);
 assert.match(directPositionBoardNote(withByte(CB_EXAMPLE,9,2)),/当前实际位置/);
 // Only CB can limit the current of one command: FB has no such field.
 assert.match(directPositionBoardNote(FB_EXAMPLE),/FB 没有电流字段：无法对单条命令限流/);
 assert.match(directPositionBoardNote(CB_EXAMPLE),/CB 的电流字段/);
 assert.equal(directPositionBoardNote([1,0xcd,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0x6b]),null);
 assert.equal(directPositionBoardNote([]),null);
});
