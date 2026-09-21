import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ADDRESS_MAX,
  COMMAND_ITEMS,
  COMMAND_GROUPS,
  PAYLOAD_BYTES_PER_FRAME,
  PROTOCOL_CHECKSUM,
  buildFrames,
  defaultValues,
  encodeCommand,
  formatCanId,
  frameAnnotation,
  getCommandItem,
  getVariant,
  identifyCommandBytes,
  parseLogicalHex,
  searchCommands,
  validateAddress,
} from '../src/protocol.js';

import {
  ACK_STATUS,
  MANUAL_DEFAULTS,
  SEED_FEEDBACK,
  classifyAck,
  createSeedMotor,
  decodeFeedbackFrame,
  encodeCurrentFrame,
  encodePositionFrame,
  encodeVelocityFrame,
  expectedDurationMs,
  parseAckFrame,
  planCommandEffect,
  planManualMove,
} from '../src/simulation.js';

const encode = (id, values = null, address = 1, variantKey = 'base') => {
  const item = getCommandItem(id);
  assert.ok(item, `catalog item ${id} exists`);
  return encodeCommand({
    item,
    variantKey,
    values: values ?? defaultValues(item, variantKey),
    address,
  });
};

const allVariants = () =>
  COMMAND_ITEMS.flatMap((item) =>
    item.variants.map((variant) => ({ item, variant, variantKey: variant.key })),
  );

test('F3 enable example matches the reference design byte for byte', () => {
  const result = encode('enable', { state: 1, sync: 0 });
  assert.equal(result.ok, true);
  assert.deepEqual(result.bytes, [0x01, 0xf3, 0xab, 0x01, 0x00, 0x6b]);
  assert.equal(result.bytes.map((b) => b.toString(16).padStart(2, '0')).join(' '), '01 f3 ab 01 00 6b');

  const frames = buildFrames(result.bytes);
  assert.equal(frames.length, 1);
  assert.equal(frames[0].canId, 0x00000100);
  assert.equal(formatCanId(frames[0].canId), '0x00000100');
  assert.equal(frames[0].dlc, 5);
  assert.deepEqual(frames[0].data, [0xf3, 0xab, 0x01, 0x00, 0x6b]);
  assert.equal(frames[0].packetIndex, 0);
  assert.equal(frames[0].extended, true);

  assert.equal(
    frameAnnotation(result.bytes, result.labels, frames[0]),
    'F3 功能码 · AB 标识 · 01 使能 · 00 同步 · 6B 固定校验',
  );
});

test('sendCommand split: CD position command becomes three 7-byte packets', () => {
  const values = { dir: 0, accel: 60, decel: 60, vel: 600, clk: 1800, motionMode: 2, sync: 0, maxCurrentMa: 1000 };
  const result = encode('position', values, 1, 'limit');
  assert.equal(result.ok, true);
  assert.equal(result.bytes.length, 18);
  assert.equal(result.bytes[1], 0xcd);
  assert.equal(result.bytes[17], PROTOCOL_CHECKSUM);

  const frames = buildFrames(result.bytes);
  assert.equal(frames.length, 3);
  assert.deepEqual(frames.map((f) => f.canId), [0x00000100, 0x00000101, 0x00000102]);
  assert.deepEqual(frames.map((f) => f.dlc), [8, 8, 3]);
  for (const frame of frames) {
    assert.equal(frame.data[0], 0xcd, 'every packet repeats the function code');
    assert.equal(frame.data.length, frame.dlc);
  }
  assert.deepEqual(frames[0].data, [0xcd, 0x00, 0x00, 0x3c, 0x00, 0x3c, 0x02, 0x58]);
  assert.deepEqual(frames[1].data, [0xcd, 0x00, 0x00, 0x07, 0x08, 0x02, 0x00, 0x03]);
  assert.deepEqual(frames[2].data, [0xcd, 0xe8, PROTOCOL_CHECKSUM]); // current low byte + checksum

  // Reassembling the packets must reproduce the original payload exactly.
  const reassembled = frames.flatMap((frame) => frame.data.slice(1));
  assert.deepEqual(reassembled, result.bytes.slice(2));

  // Packet splitting is by 7 bytes, never more.
  for (const frame of frames) {
    assert.ok(frame.data.length - 1 <= PAYLOAD_BYTES_PER_FRAME);
  }
});

test('every catalog command encodes valid bytes and valid frames', () => {
  const variants = allVariants();
  assert.ok(variants.length >= 25, `catalog exposes enough commands (${variants.length})`);
  for (const { item, variant, variantKey } of variants) {
    if (item.custom) continue;
    const values = defaultValues(item, variantKey);
    const result = encodeCommand({ item, variantKey, values, address: 7 });
    assert.equal(result.ok, true, `${item.id}/${variantKey} encodes with defaults`);
    assert.equal(result.bytes[0], 7, `${item.id} puts the address first`);
    assert.equal(result.bytes[1], variant.opcode, `${item.id} uses its function code`);
    assert.equal(result.bytes[result.bytes.length - 1], PROTOCOL_CHECKSUM, `${item.id} ends with 6B`);
    assert.ok(result.bytes.length >= 3);
    assert.equal(result.labels.length, result.bytes.length, `${item.id} labels every byte`);

    const frames = buildFrames(result.bytes);
    assert.ok(frames.length >= 1);
    frames.forEach((frame, index) => {
      assert.equal(frame.packetIndex, index);
      assert.equal(frame.canId, (7 << 8) | index, `${item.id} packet ${index} CAN id`);
      assert.equal(frame.dlc, frame.data.length);
      assert.ok(frame.dlc >= 1 && frame.dlc <= 8, `${item.id} DLC within a CAN frame`);
      assert.equal(frame.data[0], variant.opcode);
      frame.sourceIndexes.forEach((sourceIndex, position) => {
        assert.equal(frame.data[position], result.bytes[sourceIndex]);
      });
    });
  }
});

test('catalog covers every public motor command from X42sProtocol.h', () => {
  const ids = new Set(COMMAND_ITEMS.map((item) => item.id));
  for (const id of [
    'resetCurPosToZero',
    'resetClogProtection',
    'modifyCtrlMode',
    'realtimePositionFeedback',
    'enable',
    'torque',
    'velocity',
    'passthroughPosition',
    'position',
    'stop',
    'syncTrigger',
    'originSetZero',
    'originModifyParams',
    'originTriggerReturn',
    'originInterrupt',
    'customFrame',
  ]) {
    assert.ok(ids.has(id), `catalog contains ${id}`);
  }

  // All fourteen readSysParams options are exposed with their firmware code.
  const readItems = COMMAND_ITEMS.filter((item) => item.groupId === 'read');
  assert.equal(readItems.length, 14);
  const codes = readItems.map((item) => item.variants[0].opcode.toString(16));
  assert.deepEqual(
    codes,
    ['1f', '20', '21', '24', '27', '31', '33', '35', '36', '37', '3a', '3b', '42', '43'],
  );
  const conf = encode('readConf');
  assert.deepEqual(conf.bytes, [0x01, 0x42, 0x6c, 0x6b]);
  const state = encode('readState');
  assert.deepEqual(state.bytes, [0x01, 0x43, 0x7a, 0x6b]);
});

test('read commands produce the minimal two-byte CAN payload', () => {
  const result = encode('readCpos');
  assert.deepEqual(result.bytes, [0x01, 0x36, 0x6b]);
  const frames = buildFrames(result.bytes);
  assert.equal(frames.length, 1);
  assert.deepEqual(frames[0].data, [0x36, 0x6b]);
  assert.equal(frames[0].dlc, 2);
  assert.equal(frames[0].canId, 0x00000100);
});

test('20 byte origin parameter command needs three packets', () => {
  // X42sProtocol::originModifyParams: addr + 0x4C + 0xAE + save + mode + dir +
  // vel(2) + timeoutMs(4) + stallVel(2) + stallMa(2) + stallMs(2) +
  // powerOnTrigger + 0x6B == 20 bytes, so the payload is 18 bytes.
  const result = encode('originModifyParams');
  assert.equal(result.bytes.length, 20);
  assert.equal(result.bytes[1], 0x4c);
  assert.equal(result.bytes[2], 0xae);
  assert.equal(result.bytes[19], PROTOCOL_CHECKSUM);

  const frames = buildFrames(result.bytes);
  assert.equal(frames.length, 3);
  assert.deepEqual(frames.map((frame) => frame.dlc), [8, 8, 5]);
  const reassembled = frames.flatMap((frame) => frame.data.slice(1));
  assert.deepEqual(reassembled, result.bytes.slice(2));
});

test('address validation rejects out of range and non integer input', () => {
  assert.equal(validateAddress('1').ok, true);
  assert.equal(validateAddress(String(ADDRESS_MAX)).value, 255);
  assert.equal(validateAddress('0').ok, false);
  assert.equal(validateAddress('256').ok, false);
  assert.equal(validateAddress('').ok, false);
  assert.equal(validateAddress('1.5').ok, false);
  assert.equal(validateAddress('abc').ok, false);
  assert.equal(validateAddress('-3').ok, false);
  assert.equal(validateAddress('0x02').ok, false);

  const bad = encodeCommand({
    item: getCommandItem('enable'),
    variantKey: 'base',
    values: { state: 1, sync: 0 },
    address: 0,
  });
  assert.equal(bad.ok, false);
  assert.match(bad.errors.address, /1\.\.255/);
});

test('integer fields reject overflow, decimals and non finite input without clamping', () => {
  const item = getCommandItem('position');
  const base = { dir: 0, accel: 60, decel: 60, vel: 600, clk: 1800, motionMode: 2, sync: 0, maxCurrentMa: 1000 };
  const run = (patch) => encodeCommand({
    item,
    variantKey: 'limit',
    values: { ...base, ...patch },
    address: 1,
  });

  assert.equal(run({}).ok, true);

  const overflow = run({ accel: 65536 });
  assert.equal(overflow.ok, false);
  assert.match(overflow.errors.accel, /不做静默截断/);

  assert.equal(run({ accel: 65535 }).ok, true, 'upper bound is allowed');
  assert.equal(run({ accel: -1 }).ok, false);
  assert.equal(run({ accel: '1.5' }).ok, false);
  assert.match(run({ accel: '1.5' }).errors.accel, /整数/);
  assert.equal(run({ accel: 'NaN' }).ok, false);
  assert.equal(run({ accel: 'Infinity' }).ok, false);
  assert.equal(run({ accel: '' }).ok, false);
  assert.equal(run({ clk: 4294967296 }).ok, false, 'uint32 overflow rejected');
  assert.equal(run({ clk: 4294967295 }).ok, true);
  assert.equal(run({ motionMode: 256 }).ok, false);
  assert.equal(run({ dir: 2 }).ok, false, 'direction is a 0/1 field');

  const current = run({ maxCurrentMa: 5001 });
  assert.equal(current.ok, false, 'firmware bound 0..5000 is enforced instead of clamping');
  assert.match(current.errors.maxCurrentMa, /5000/);
  assert.equal(run({ maxCurrentMa: 5000 }).ok, true);

  const encoded = run({ accel: 65535, maxCurrentMa: 5000 }).bytes;
  assert.deepEqual(encoded.slice(3, 5), [0xff, 0xff], 'big endian word encoding');
});

test('raw logical hex parsing validates structure', () => {
  const good = parseLogicalHex('01 F3 AB 01 00 6B');
  assert.equal(good.ok, true);
  assert.deepEqual(good.bytes, [0x01, 0xf3, 0xab, 0x01, 0x00, 0x6b]);

  assert.equal(parseLogicalHex('').ok, false);
  assert.match(parseLogicalHex('01 F3 ZZ').errors[0], /十六进制/);
  assert.match(parseLogicalHex('01 F3 A').errors[0], /两位/);
  assert.match(parseLogicalHex('01 F3').errors[0], /至少需要 3 字节/);
  assert.match(parseLogicalHex('00 F3 AB 01 00 6B').errors[0], /1\.\.255/);
  assert.match(parseLogicalHex('01 F3 AB 01 00 6C').errors[0], /6B/);
});

test('custom frames only need an opcode and always append 6B', () => {
  const item = getCommandItem('customFrame');
  const result = encodeCommand({ item, variantKey: 'base', values: { opcode: 'A1', params: '02 03' }, address: 2 });
  assert.equal(result.ok, true);
  assert.deepEqual(result.bytes, [0x02, 0xa1, 0x02, 0x03, 0x6b]);

  const frames = buildFrames(result.bytes);
  assert.equal(frames.length, 1);
  assert.equal(frames[0].canId, 0x00000200);
  assert.deepEqual(frames[0].data, [0xa1, 0x02, 0x03, 0x6b]);

  assert.equal(encodeCommand({ item, variantKey: 'base', values: { opcode: '', params: '' }, address: 1 }).ok, false);
  assert.equal(encodeCommand({ item, variantKey: 'base', values: { opcode: 'GG', params: '' }, address: 1 }).ok, false);
  assert.equal(encodeCommand({ item, variantKey: 'base', values: { opcode: 'A1', params: '2' }, address: 1 }).ok, false);

  // sendCommand's length argument is a uint8_t: 255 logical bytes is the ceiling.
  const atLimit = encodeCommand({
    item,
    variantKey: 'base',
    values: { opcode: 'A1', params: '00 '.repeat(252).trim() },
    address: 1,
  });
  assert.equal(atLimit.ok, true);
  assert.equal(atLimit.bytes.length, 255);

  const overLimit = encodeCommand({
    item,
    variantKey: 'base',
    values: { opcode: 'A1', params: '00 '.repeat(253).trim() },
    address: 1,
  });
  assert.equal(overLimit.ok, false);
  assert.match(overLimit.errors.params, /255/);
});

test('feedback decoders follow MotionCore.h and reject malformed frames', () => {
  const position = decodeFeedbackFrame([...SEED_FEEDBACK]);
  assert.equal(position.field, 'position');
  assert.equal(position.value, 1800);
  assert.equal(position.text, '位置 180.0°');

  const negative = decodeFeedbackFrame(encodePositionFrame(-1800));
  assert.equal(negative.value, -1800);

  const velocity = decodeFeedbackFrame(encodeVelocityFrame(600));
  assert.equal(velocity.field, 'velocity');
  assert.equal(velocity.value, 600);
  assert.equal(velocity.text, '速度 60.0 RPM');

  const current = decodeFeedbackFrame(encodeCurrentFrame(210));
  assert.equal(current.field, 'current');
  assert.equal(current.value, 210);

  const flags = decodeFeedbackFrame([0x3a, 0x01, 0x6b]);
  assert.equal(flags.field, 'flags');
  assert.equal(flags.value, 1);

  assert.equal(decodeFeedbackFrame([0x36, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00]), null, 'checksum');
  assert.equal(decodeFeedbackFrame([0x36, 0x02, 0x00, 0x00, 0x07, 0x08, 0x6b]), null, 'sign byte');
  assert.equal(decodeFeedbackFrame([0x36, 0x00, 0x00, 0x07, 0x08, 0x6b]), null, 'length');
  assert.equal(decodeFeedbackFrame([0x11, 0x22, 0x6b]), null, 'unknown opcode');
});

test('acknowledgement frames use the firmware status bytes', () => {
  const ack = parseAckFrame([0xf3, 0x02, 0x6b]);
  assert.equal(ack.status, ACK_STATUS.received);
  assert.equal(ack.text, '已接收指令');
  assert.equal(parseAckFrame([0xfe, 0x9f, 0x6b]).text, '已完成');
  assert.equal(parseAckFrame([0xfe, 0xe2, 0x6b]).text, '参数错误');
  assert.equal(parseAckFrame([0xfe, 0xee, 0x6b]).text, '格式错误');
  assert.equal(classifyAck(0x99), '未知状态 0x99');
  assert.equal(parseAckFrame([0xf3, 0x02, 0x00]), null);
  assert.equal(parseAckFrame([0xf3, 0x02]), null);
});

test('trapezoid duration matches MotionCore for the CD move path', () => {
  // 180.0 degree at 60 RPM (360 deg/s) with 60 RPM/s (360 deg/s^2) ramps needs
  // 180 degree to accelerate and 180 degree to decelerate, so the profile is
  // triangular: peak = sqrt(2*180*360*360/(360+360)) = 254.56 deg/s and
  // t = 2 * peak / accel = 1.4142 s -> ceil(1414.2) = 1415 ms.
  const triangular = expectedDurationMs(1800, 600, 60, 60);
  assert.equal(triangular, Math.ceil(Math.sqrt(2) * 1000));
  assert.equal(triangular, 1415);

  // A longer travel with the same ramps reaches cruise speed (trapezoid):
  // 450 degree = 1 s accel + 1 s decel + 0.25 s cruise at 360 deg/s.
  const trapezoid = expectedDurationMs(4500, 600, 60, 60);
  assert.equal(trapezoid, 2250);

  assert.equal(expectedDurationMs(1800, 0, 60, 60), 0);
  assert.equal(expectedDurationMs(0, 600, 60, 60), 0);
  assert.equal(expectedDurationMs(1800, 600, 0, 60), 0);
});

test('command effects describe simulation behaviour without inventing hardware', () => {
  const enable = planCommandEffect({
    item: getCommandItem('enable'),
    variantKey: 'base',
    values: { state: 1, sync: 0 },
  });
  assert.equal(enable.kind, 'enable');
  assert.equal(enable.enabled, true);
  assert.equal(enable.ackStatus, ACK_STATUS.received);

  const queued = planCommandEffect({
    item: getCommandItem('position'),
    variantKey: 'limit',
    values: { dir: 1, accel: 60, decel: 60, vel: 600, clk: 1800, motionMode: 2, sync: 1, maxCurrentMa: 1000 },
  });
  assert.equal(queued.sync, true, 'sync option queues instead of moving');
  assert.equal(queued.cancelsMotion, true);
  assert.equal(queued.motion.deltaTenths, -1800, 'direction byte flips the travel sign');
  assert.equal(queued.motion.durationMs, 1415);

  // Only the relative trapezoid mode (MotionCore kMotionModeRelativeToCurrent)
  // is modelled; other mode bytes keep the frames but are not simulated.
  const unknownMode = planCommandEffect({
    item: getCommandItem('position'),
    variantKey: 'limit',
    values: { dir: 0, accel: 60, decel: 60, vel: 600, clk: 1800, motionMode: 5, sync: 0, maxCurrentMa: 1000 },
  });
  assert.equal(unknownMode.kind, 'not-modelled');
  assert.equal(unknownMode.ackStatus, null, 'no fabricated completion for unknown modes');
  assert.equal(unknownMode.motion, undefined);

  // Passthrough travel is carried in an undefined unit, so it is never moved.
  const passthrough = planCommandEffect({
    item: getCommandItem('passthroughPosition'),
    variantKey: 'base',
    values: { dir: 0, vel: 600, clk: 1800, motionMode: 2, sync: 0 },
  });
  assert.equal(passthrough.kind, 'not-modelled');
  assert.equal(passthrough.ackStatus, null);

  const velocity = planCommandEffect({
    item: getCommandItem('velocity'),
    variantKey: 'base',
    values: { dir: 0, acc: 0, vel: 600, sync: 0 },
  });
  assert.equal(velocity.motion.mode, 'continuous');

  const torque = planCommandEffect({
    item: getCommandItem('torque'),
    variantKey: 'base',
    values: { dir: 0, accel: 1000, currentMa: 800, sync: 1 },
  });
  assert.equal(torque.kind, 'torque');
  assert.equal(torque.sync, true, 'sync torque is queued like any other sync command');
  assert.equal(torque.cancelsMotion, true);

  const origin = planCommandEffect({
    item: getCommandItem('originTriggerReturn'),
    variantKey: 'base',
    values: { mode: 0, sync: 0 },
  });
  assert.equal(origin.kind, 'device-note');
  assert.match(origin.note, /模拟说明/);

  const read = planCommandEffect({
    item: getCommandItem('readCpos'),
    variantKey: 'base',
    values: {},
  });
  assert.equal(read.decoder, 'position');
  assert.equal(read.cancelsMotion, false, 'a read must not abort a running motion');

  const unknownRead = planCommandEffect({
    item: getCommandItem('readVer'),
    variantKey: 'base',
    values: {},
  });
  assert.equal(unknownRead.decoder, null);
  assert.equal(unknownRead.ackStatus, null, 'undefined response layouts are not acked or invented');

  const custom = planCommandEffect({
    item: getCommandItem('customFrame'),
    variantKey: 'base',
    values: { opcode: 'A1', params: '' },
  });
  assert.equal(custom.kind, 'unsupported');
  assert.equal(custom.ackStatus, null, 'unknown opcodes never fabricate a response');
});

test('seed motor matches the sample data in the reference design', () => {
  const motor = createSeedMotor();
  assert.equal(motor.enabled, true);
  assert.equal(motor.positionTenths, 1800);
  assert.equal(motor.currentMa, 210);
  assert.equal(motor.velocityTenths, 0);
  assert.deepEqual(motor.lastResponse.data, [0xf3, 0x02, 0x6b]);
});

test('identifyCommandBytes recovers the catalog command behind raw bytes', () => {
  const result = encode('enable', { state: 1, sync: 0 });
  const match = identifyCommandBytes(result.bytes, 1);
  assert.equal(match.item.id, 'enable');
  assert.deepEqual(match.values, { state: 1, sync: 0 });

  const read = encode('readCpos');
  assert.equal(identifyCommandBytes(read.bytes, 1).item.id, 'readCpos');
  assert.equal(identifyCommandBytes([0x01, 0xa1, 0x6b], 1), null, 'unknown opcode stays unknown');
});

test('manual move planner accepts its defaults and builds a relative CD plan', () => {
  const plan = planManualMove({ ...MANUAL_DEFAULTS });
  assert.equal(plan.ok, true, JSON.stringify(plan.errors));
  assert.equal(plan.plan.dir, 0);
  assert.equal(plan.plan.clk, 900, '90 degree is carried as 900 tenths');
  assert.equal(plan.plan.vel, 300, '30 RPM is carried as 300 tenths');
  assert.equal(plan.plan.accel, 60);
  assert.equal(plan.plan.decel, 60);
  assert.equal(plan.plan.maxCurrentMa, 800);
  assert.equal(plan.plan.motionMode, 2, 'MotionCore kMotionModeRelativeToCurrent');
  assert.equal(plan.plan.sync, 0);
  assert.equal(plan.deltaTenths, 900);
  assert.ok(plan.durationMs > 0);

  // The CD encoder accepts the planned values unchanged.
  const encoded = encodeCommand({
    item: getCommandItem('position'),
    variantKey: 'limit',
    values: plan.plan,
    address: 1,
  });
  assert.equal(encoded.ok, true, JSON.stringify(encoded.errors));
  assert.equal(encoded.bytes[1], 0xcd);
  assert.equal(encoded.bytes.length, 18);
});

test('manual move planner takes direction from the direction field only', () => {
  const forward = planManualMove({ ...MANUAL_DEFAULTS, angle: '90', dir: 0 });
  const reverse = planManualMove({ ...MANUAL_DEFAULTS, angle: '90', dir: 1 });
  assert.equal(forward.ok, true);
  assert.equal(reverse.ok, true);
  assert.equal(forward.plan.clk, reverse.plan.clk, 'travel magnitude is unchanged');
  assert.equal(forward.plan.dir, 0);
  assert.equal(reverse.plan.dir, 1);
  assert.equal(forward.deltaTenths, 900);
  assert.equal(reverse.deltaTenths, -900, 'direction flips the signed target');

  // A negative angle is rejected: the magnitude field is positive by contract.
  const negative = planManualMove({ ...MANUAL_DEFAULTS, angle: '-90' });
  assert.equal(negative.ok, false);
  assert.match(negative.errors.angle, /0\.1\.\.3600/);
});

test('manual move planner rejects raw out of range and non finite input before rounding', () => {
  const run = (patch) => planManualMove({ ...MANUAL_DEFAULTS, ...patch });

  assert.match(run({ angle: '3600.4' }).errors.angle, /0\.1\.\.3600/);
  assert.match(run({ angle: '0.04' }).errors.angle, /0\.1\.\.3600/);
  assert.equal(run({ angle: '3600' }).ok, true);
  assert.equal(run({ angle: '0.1' }).ok, true);
  assert.match(run({ angle: 'abc' }).errors.angle, /有限数字/);
  assert.match(run({ angle: 'Infinity' }).errors.angle, /有限数字/);

  assert.match(run({ speed: '120.5' }).errors.speed, /0\.1\.\.120/);
  assert.match(run({ speed: '0' }).errors.speed, /0\.1\.\.120/);
  assert.equal(run({ speed: '120' }).ok, true);

  assert.match(run({ accel: '0' }).errors.accel, /1\.\.240/);
  assert.match(run({ accel: '60.5' }).errors.accel, /整数/);
  assert.match(run({ accel: '241' }).errors.accel, /1\.\.240/);
  assert.match(run({ current: '99' }).errors.current, /100\.\.5000/);
  assert.match(run({ current: '5001' }).errors.current, /100\.\.5000/);
  assert.equal(run({ current: '5000' }).ok, true);

  // Nothing is silently clipped: the plan is absent whenever an error exists.
  const bad = run({ accel: '241' });
  assert.equal(bad.plan, null);
});

test('catalog search matches Chinese names, function codes and English terms', () => {
  const ids = (query) => searchCommands(query).map((item) => item.id);
  assert.ok(ids('使能').includes('enable'));
  assert.ok(ids('F3').includes('enable'));
  assert.ok(ids('f3').includes('enable'));
  assert.ok(ids('0x36').includes('readCpos'));
  assert.ok(ids('cpos').includes('readCpos'));
  assert.ok(ids('限流').includes('velocity'));
  assert.ok(ids('回零').includes('originTriggerReturn'));
  assert.ok(ids('position').includes('position'));
  assert.ok(ids('torque').includes('torque'));
  assert.equal(searchCommands('这个指令不存在').length, 0);
  assert.equal(searchCommands('').length, COMMAND_ITEMS.length);
});

test('catalog groups stay coherent', () => {
  const groupIds = COMMAND_GROUPS.map((group) => group.id);
  assert.deepEqual(groupIds, ['basic', 'motion', 'read', 'origin', 'config', 'custom']);
  for (const item of COMMAND_ITEMS) {
    assert.ok(item.name && item.summary, `${item.id} has UI copy`);
    assert.ok(item.variants.length >= 1);
    const variant = getVariant(item, 'missing-key');
    assert.equal(variant, item.variants[0], 'unknown variant keys fall back to the first variant');
  }
});
