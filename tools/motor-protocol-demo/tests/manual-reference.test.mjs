import assert from 'node:assert/strict';
import test from 'node:test';
import { decodeCanReply, decodeBulkCanReply } from '../src/manual-reference.js';
import { buildFrames, getCommandItem, defaultValues, encodeCommand, searchCommands } from '../src/protocol.js';

const frame = (data, extra={}) => ({dir:'RX',id:0x100,extended:true,remote:false,data,...extra});
const text = data => {const reply=decodeCanReply(frame(data));assert.ok(reply);return reply.text;};

test('V1.0.5 p42 CAN example repeats opcode in the second packet', () => {
  const frames=buildFrames([1,0xfd,1,0x0f,0xa0,0,0,1,0xfa,0,0,0,0x6b]);
  assert.deepEqual(frames.map(f=>f.data),[[0xfd,1,0x0f,0xa0,0,0,1,0xfa],[0xfd,0,0,0,0x6b]]);
});
test('read payload 02 is voltage data, not an acceptance ACK', () => {
  assert.match(text([0x24,2,0,0x6b]), /512/);
  assert.doesNotMatch(text([0x24,2,0,0x6b]), /接收命令|已接收|已接受/);
});

test('multi-packet X configuration returns its actual parameter values', () => {
  const logical = [0x42,0x25,0x18,0x00,0x01,0x01,0x02,0x02,0x00,0x10,0x01,0x00,0x00,0x04,0xb0,0x0b,0x80,0x0b,0xb8,0x03,0xe8,0x05,0x07,0x00,0x01,0x00,0x01,0x00,0x08,0x08,0x98,0x07,0xd0,0x00,0x08,0x6b];
  const packets = [];
  for (let index=0; index*7 < logical.length-1; index++) packets.push(frame([0x42,...logical.slice(1+index*7,1+(index+1)*7)],{id:0x100+index}));
  const reply = decodeBulkCanReply(packets);
  assert.ok(reply);
  assert.match(reply.text,/闭环模式最大电流 2944 mA/); // 0x0B80; the manual prose incorrectly calls it 3000.
  assert.match(reply.text,/控制命令应答.*Receive/);
  assert.equal(decodeBulkCanReply(packets.slice(0,-1)),null);
});

test('multi-packet X status is decoded only with intact packet order and sign bytes', () => {
  const logical = [0x43,0x25,0x0c,0x5c,0x67,0x00,0x00,0x00,0x03,0x30,0xb5,0x43,0xeb,0x01,0x00,0x00,0x0e,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x0e,0x10,0x01,0x00,0x00,0x00,0x02,0x00,0x22,0x03,0x03,0x6b];
  const packets = [];
  for (let index=0;index<5;index++) packets.push(frame([0x43,...logical.slice(1+index*7,1+(index+1)*7)],{id:0x100+index}));
  const reply = decodeBulkCanReply(packets);
  assert.ok(reply);
  assert.match(reply.text,/目标位置 -360.0°/);
  assert.match(reply.text,/实时位置 360.0°/);
  assert.match(reply.text,/位置误差 -0.02°/);
  assert.equal(decodeBulkCanReply([packets[0],packets[2],packets[1],packets[3],packets[4]]),null);
  assert.equal(decodeBulkCanReply(packets.map((p,i)=>i===4?{...p,data:[...p.data.slice(0,-1),0]}:p)),null);
});
test('manual temperature sign differs from signed X position error', () => {
  assert.match(text([0x39,0,25,0x6b]), /-25/);
  assert.match(text([0x39,1,25,0x6b]), /25/);
  assert.doesNotMatch(text([0x39,1,25,0x6b]), /-25/);
  assert.match(text([0x37,1,0,0,0,8,0x6b]), /-0\.08/);
});
test('bad or partial frames cannot become telemetry', () => {
  for (const extra of [{dir:'TX'},{extended:false},{remote:true},{id:0x101},{id:0},{id:0x10100}])
    assert.equal(decodeCanReply(frame([0x24,2,0,0x6b],extra)),null);
  for(const data of [[0x24,2,0],[0x24,2,0,0],[0x24,2,0,0,0x6b],[0x39,2,25,0x6b],[0x37,4,0,0,0,8,0x6b]])
    assert.equal(decodeCanReply(frame(data)),null);
});
test('new diagnostic queries have no extra payload', () => {
  for(const [id,op] of [['readCbus',0x26],['readPulses',0x32],['readSetTarget',0x34],['readTemp',0x39],['readFlags',0x3c],['readIo',0x3d]]) {
    const item=getCommandItem(id);assert.ok(item,id);
    const result=encodeCommand({item,variantKey:'base',values:defaultValues(item,'base'),address:7});
    assert.equal(result.ok,true);assert.deepEqual(result.bytes,[7,op,0x6b]);
  }
});
test('control ACK and homing bits do not manufacture completion', () => {
  assert.match(text([0xf3,2,0x6b]), /不代表.*完成/);
  assert.match(text([0xf3,0x12,0x6b]),/未说明|未知|原样/);
  assert.match(text([0x9a,0x12,0x6b]), /不动|不执行/);
  const flags=text([0x3c,0,0x33,0x6b]);
  assert.match(flags,/未.*回零|不在回零/);
  assert.match(flags,/高电平/);
  assert.doesNotMatch(flags,/回零成功|已完成回零/);
  assert.doesNotMatch(text([0x3b,0x0c,0x6b]), /状态.*0x00|未在回零且未报失败/);
});
test('closed-loop current limit 0x45 encodes the manual examples, not a readback', () => {
  const item=getCommandItem('closedLoopCurrentLimit');
  assert.ok(item,'closedLoopCurrentLimit exists');
  const encode=(values)=>encodeCommand({
    item,variantKey:'base',
    values:{...defaultValues(item,'base'),...values},address:1,
  }).bytes;
  // Manual V1.0.5 p82: 01 45 66 00 00 78 6B (120 mA, unsaved) and
  // 01 45 66 01 00 64 6B (100 mA, saved).
  assert.deepEqual(encode({save:0,currentMa:120}),[0x01,0x45,0x66,0x00,0x00,0x78,0x6b]);
  assert.deepEqual(encode({save:1,currentMa:100}),[0x01,0x45,0x66,0x01,0x00,0x64,0x6b]);
  // The 120 mA default is an editor example: nothing reads the driver's value.
  assert.equal(defaultValues(item,'base').save,0);
  assert.equal(item.variants[0].layout.find((f)=>f.key==='currentMa').default,120);
  assert.equal(item.variants[0].layout.find((f)=>f.key==='currentMa').max,5000);
  assert.equal(item.variants[0].layout.find((f)=>f.key==='currentMa').min,0);
  // The reply is the documented control answer and nothing more.
  assert.match(text([0x45,2,0x6b]),/接收的命令正确/);
  assert.match(text([0x45,2,0x6b]),/不代表.*完成/);
  assert.match(text([0x45,0xe2,0x6b]),/参数错误/);
  assert.equal(decodeCanReply(frame([0x45,2,0,0x6b])),null,'0x45 has no data reply');
});
test('0x45 is reachable by opcode, Chinese name and the English hint', () => {
  for(const query of ['45','闭环','闭环最大相电流','closed loop'])
    assert.ok(searchCommands(query).some((item)=>item.id==='closedLoopCurrentLimit'),query);
});
test('the homing collision threshold stays a threshold, not a current limit', () => {
  const stall=getCommandItem('originModifyParams').variants[0].layout.find((f)=>f.key==='stallMa');
  assert.equal(stall.label,'碰撞检测电流阈值');
  assert.match(stall.hint,/不是限流值/);
  assert.match(stall.hint,/0x45/);
});
test('manual enums reject unassigned modes before raw send', () => {
  for(const [id,key,value] of [['modifyCtrlMode','ctrlMode',2],['originTriggerReturn','mode',6],['position','motionMode',3]]) {
    const item=getCommandItem(id), values={...defaultValues(item,'base'),[key]:value};
    assert.equal(encodeCommand({item,variantKey:'base',values,address:1}).ok,false);
  }
  const home=getCommandItem('originModifyParams');
  assert.equal(home.variants[0].layout.find(f=>f.key==='vel').unit,'RPM');
});
