// Unit tests for the local draft store behind the device page's forms.
//
// The store is pure and storage is injected, so every failure mode a browser can
// produce (no storage, blocked storage, foreign JSON, another version, oversized
// or hostile payloads) is exercised here without a browser.
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  DRAFTS_KEY, DRAFTS_VERSION, FIELD_LIMIT, FORM_LIMIT, RAW_TEXT_MAX, VALUE_TEXT_MAX,
  emptyDrafts, getForm, getManual, lastVariantFor, pickFields, putForm,
  putManual, putSelection, readDrafts, sanitizeFields, writeDrafts,
} from '../src/device-drafts.js';

const memoryStorage = (initial = null) => {
  const map = new Map();
  if (initial !== null) map.set(DRAFTS_KEY, initial);
  return {
    map,
    getItem: (key) => (map.has(key) ? map.get(key) : null),
    setItem: (key, value) => map.set(key, String(value)),
    removeItem: (key) => map.delete(key),
  };
};
// A storage that throws on every access, like a blocked origin.
const hostileStorage = () => ({
  getItem() { throw new Error('denied'); },
  setItem() { throw new Error('denied'); },
  removeItem() { throw new Error('denied'); },
});

test('form drafts round-trip per motor, command and variant', () => {
  const storage = memoryStorage();
  let drafts = emptyDrafts();
  drafts = putForm(drafts, 1, 'position', 'limit', {values:{accel:'1000', clk:'1800', motionMode:2}});
  drafts = putForm(drafts, 1, 'position', 'base', {values:{accel:'60'}, raw:'01 FD 00', dirty:true, mode:'raw'});
  drafts = putForm(drafts, 2, 'position', 'limit', {values:{accel:'7'}});
  drafts = putSelection(drafts, {motorId:'2', commandId:'position', variantKey:'limit'});
  assert.equal(writeDrafts(storage, drafts), true);

  const restored = readDrafts(storage);
  assert.equal(restored.version, DRAFTS_VERSION);
  assert.equal(restored.lastMotorId, '2');
  assert.equal(restored.lastCommandId, 'position');
  assert.equal(restored.lastVariantKey, 'limit');
  assert.deepEqual(getForm(restored, 1, 'position', 'limit').values, {accel:'1000', clk:'1800', motionMode:2});
  // Variants are independent: the FB draft never leaks into the CB one.
  assert.deepEqual(getForm(restored, 1, 'position', 'base').values, {accel:'60'});
  assert.equal(getForm(restored, 1, 'position', 'base').dirty, true);
  assert.equal(getForm(restored, 1, 'position', 'base').raw, '01 FD 00');
  assert.equal(getForm(restored, 1, 'position', 'base').mode, 'raw');
  // Motors are independent: motor 2 kept its own value.
  assert.deepEqual(getForm(restored, 2, 'position', 'limit').values, {accel:'7'});
  assert.equal(getForm(restored, 3, 'position', 'limit'), null, 'unknown key -> defaults');
  assert.equal(getForm(restored, 1, 'velocity', 'base'), null, 'other commands stay isolated');
});

test('exact user input is preserved, including partial and invalid text', () => {
  let drafts = emptyDrafts();
  const typed = {angle:'', speed:'1.', accel:'-', decel:'0x1F', current:' 300 ', motionMode:'2'};
  drafts = putForm(drafts, 1, 'position', 'limit', {values:typed});
  assert.deepEqual(getForm(drafts, 1, 'position', 'limit').values, typed);
  // Values that are not primitives are dropped instead of being coerced.
  assert.deepEqual(sanitizeFields({accel:'60', nested:{a:1}, list:[1], nothing:null, nan:NaN}), {accel:'60'});
  assert.deepEqual(pickFields({accel:'60', clk:'1800'}, ['clk']), {clk:'1800'});
  assert.deepEqual(pickFields({accel:'60'}, ['clk']), {});
});

test('the last used variant of a command is remembered', () => {
  let drafts = emptyDrafts();
  drafts = putForm(drafts, 1, 'passthroughPosition', 'base', {values:{vel:'300'}});
  assert.equal(lastVariantFor(drafts, 1, 'passthroughPosition'), 'base');
  drafts = putForm(drafts, 1, 'passthroughPosition', 'limit', {values:{vel:'600'}});
  assert.equal(lastVariantFor(drafts, 1, 'passthroughPosition'), 'limit');
  // A selection without edits also counts, so the page returns to what was open.
  drafts = putForm(drafts, 1, 'position', 'base', {});
  assert.equal(lastVariantFor(drafts, 1, 'position'), 'base');
  assert.equal(getForm(drafts, 1, 'position', 'base').values, null);
  assert.equal(lastVariantFor(drafts, 1, 'velocity'), null);
  assert.equal(lastVariantFor(drafts, 2, 'passthroughPosition'), null, 'other motors stay isolated');
});

test('trial-field drafts are kept per motor and survive a reload', () => {
  const storage = memoryStorage();
  let drafts = emptyDrafts();
  drafts = putManual(drafts, 1, {dir:0, angle:'120', speed:'10', accel:'60', decel:'60', current:'300'});
  drafts = putManual(drafts, 2, {dir:1, angle:'15'});
  assert.deepEqual(getManual(drafts, 1), {dir:0, angle:'120', speed:'10', accel:'60', decel:'60', current:'300'});
  assert.deepEqual(getManual(drafts, 2), {dir:1, angle:'15'});
  assert.equal(getManual(drafts, 3), null);
  assert.equal(getManual(drafts, '2').angle, '15', 'ids are looked up as strings');
  writeDrafts(storage, drafts);
  assert.deepEqual(getManual(readDrafts(storage), 2), {dir:1, angle:'15'});
});

test('a corrupt, foreign or unreadable payload degrades to safe defaults', () => {
  for (const payload of [
    '', 'not json', '[]', 'null', '42', '"text"',
    JSON.stringify({version:DRAFTS_VERSION + 1, forms:{'1|enable|base':{values:{a:1}}}}),
    JSON.stringify({forms:{'1|enable|base':{values:{accel:'60'}}}}),  // no version
    JSON.stringify({version:DRAFTS_VERSION, forms:'nope', manuals:[1,2], lastMotorId:{}}),
    JSON.stringify({version:DRAFTS_VERSION, manuals:{'1':'nope', '':{values:{a:'1'}}}}),
  ]) {
    const drafts = readDrafts(memoryStorage(payload));
    assert.equal(drafts.version, DRAFTS_VERSION);
    assert.deepEqual(drafts.forms, {}, payload);
    assert.deepEqual(drafts.manuals, {}, payload);
    assert.equal(drafts.lastMotorId, '');
  }
  // A single unusable entry inside an otherwise valid payload is neutralized
  // without discarding the rest of the store.
  const partial = readDrafts(memoryStorage(JSON.stringify({
    version:DRAFTS_VERSION,
    forms:{'1|x|y':{values:'nope'}},
    manuals:{'1':{values:{angle:'7'}}},
  })));
  assert.equal(getForm(partial, 1, 'x', 'y').values, null);
  assert.deepEqual(getManual(partial, 1), {angle:'7'});
  // No storage at all, and a storage that throws, are equally safe.
  assert.deepEqual(readDrafts(null).forms, {});
  assert.deepEqual(readDrafts(hostileStorage()).forms, {});
  assert.equal(writeDrafts(hostileStorage(), emptyDrafts()), false);
  assert.equal(writeDrafts(null, emptyDrafts()), false);
  // A blocked origin still yields usable in-memory drafts.
  const drafts = putForm(readDrafts(hostileStorage()), 1, 'enable', 'base', {values:{state:'1'}});
  assert.deepEqual(getForm(drafts, 1, 'enable', 'base').values, {state:'1'});
});

test('only editable primitives are stored, never board state', () => {
  let drafts = emptyDrafts();
  // A value map as the page builds it, plus board state a hostile caller might
  // add: objects and arrays are dropped outright.
  drafts = putForm(drafts, 1, 'enable', 'base', {values:{
    state:'1',
    limits:{maxSpeedRpm:120}, faults:['none'], ack:{status:'received'},
  }});
  const values = getForm(drafts, 1, 'enable', 'base').values;
  assert.equal(values.state, '1');
  assert.equal(values.limits, undefined);
  assert.equal(values.faults, undefined);
  assert.equal(values.ack, undefined);
  assert.doesNotMatch(JSON.stringify(drafts), /maxSpeedRpm|received/);
  // Restoring only ever reads the keys of the variant being shown, so anything
  // else a draft happens to carry is ignored by the page.
  assert.deepEqual(pickFields({state:'1', password:'hunter2', enabled:true}, ['state']), {state:'1'});
  // An empty trial draft stores nothing at all.
  assert.equal(getManual(drafts, 1), null);
  assert.equal(JSON.stringify(putManual(emptyDrafts(), 1, {})), JSON.stringify(emptyDrafts()));
});

test('stored data stays bounded', () => {
  let drafts = emptyDrafts();
  for (let i = 0; i < FORM_LIMIT + 12; ++i)
    drafts = putForm(drafts, 1, `command${i}`, 'base', {values:{accel:'60'}});
  assert.equal(Object.keys(drafts.forms).length, FORM_LIMIT);
  assert.notEqual(getForm(drafts, 1, `command${FORM_LIMIT + 11}`, 'base'), null);
  assert.equal(getForm(drafts, 1, 'command0', 'base'), null, 'oldest entries are dropped first');

  const long = 'a'.repeat(500);
  drafts = putForm(drafts, 1, 'enable', 'base', {raw:'b'.repeat(2000), values:{state:long}});
  assert.equal(getForm(drafts, 1, 'enable', 'base').raw.length, RAW_TEXT_MAX);
  assert.equal(getForm(drafts, 1, 'enable', 'base').values.state.length, VALUE_TEXT_MAX);
  const many = {};
  for (let i = 0; i < 100; ++i) many[`k${i}`] = '1';
  assert.equal(Object.keys(sanitizeFields(many)).length, FIELD_LIMIT);
});
