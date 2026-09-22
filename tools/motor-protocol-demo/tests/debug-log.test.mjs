// Unit tests for the diagnostic log store.
//
// The store takes its clock, storage and timers injected, so everything that
// matters can be checked without a browser: bounded history, corrupt/denied
// persistence, request redaction (including nested credentials), export
// escaping, board dedup/restart/gap, and the promise that the logger can never
// change a request's outcome.
import assert from 'node:assert/strict';
import test from 'node:test';
import {
  LOG_STORAGE_KEY, LOG_VERSION, MAX_EVENTS, MAX_TRACE_FRAMES, PERSISTED_PROGRAMS,
  PROGRAM_TEXT_MAX, SAVE_DEBOUNCE_MS,
  boardDelta, createLogStore, diffFields, emptyState, exportJson, exportMarkdown, formatChanges,
  isPeriodicQueryFrame, limitsKey, parseBoardLog, programSummary, queueKey, readPersisted,
  redactObject, sanitizeParams, statusKey, summarizeBody, writePersisted,
} from '../src/debug-log.js';

const memoryStorage = (initial = null) => {
  const map = new Map();
  if (initial !== null) map.set(LOG_STORAGE_KEY, initial);
  return {
    map,
    getItem: (key) => (map.has(key) ? map.get(key) : null),
    setItem: (key, value) => map.set(key, String(value)),
    removeItem: (key) => map.delete(key),
  };
};
const deniedStorage = () => ({
  getItem() { throw new Error('denied'); },
  setItem() { throw new Error('denied'); },
  removeItem() { throw new Error('denied'); },
});

// A store with a deterministic clock and an explicit timer, so the debounced
// save can be flushed on demand instead of waiting.
function makeStore({ storage = memoryStorage(), start = 1_700_000_000_000 } = {}) {
  let now = start;
  const timers = [];
  const store = createLogStore({
    storage,
    wallClock: () => now,
    setTimer: (fn, ms) => { timers.push({ fn, ms }); return timers.length - 1; },
    clearTimer: (handle) => { if (timers[handle]) timers[handle] = null; },
  });
  return {
    store,
    storage,
    advance: (ms) => { now += ms; },
    runTimers: () => { for (const timer of timers) if (timer) timer.fn(); timers.length = 0; },
  };
}

const boardPayload = (bootId, sequence, events) => ({ bootId, sequence, capacity: 48, uptimeMs: 5000, events });

test('events are bounded, ordered and persisted without secrets', () => {
  const { store, storage, runTimers } = makeStore();
  for (let i = 0; i < MAX_EVENTS + 25; ++i) store.add({ source: 'session', event: `e${i}`, detail: `d${i}` });
  const snapshot = store.snapshot();
  assert.equal(snapshot.events.length, MAX_EVENTS);
  assert.equal(snapshot.events[0].event, 'e25', 'oldest entries are dropped first');
  assert.equal(snapshot.events.at(-1).event, `e${MAX_EVENTS + 24}`);
  assert.equal(store.flush(), true);
  const restored = readPersisted(storage);
  assert.equal(restored.events.length, MAX_EVENTS);
  assert.equal(restored.version, LOG_VERSION);
});

test('request inputs are allowlisted, redacted and bounded', () => {
  // Only the documented parameters of that route are kept.
  assert.deepEqual(sanitizeParams('/api/command', {hex:'01F3AB01006B', password:'hunter2', extra:'x'}), {hex:'01F3AB01006B'});
  assert.deepEqual(sanitizeParams('/api/queue/start', {program:'move 1 90', repeat:'2'}), {repeat:'2',program:'move 1 90',programTruncated:false});
  assert.deepEqual(sanitizeParams('/api/wifi/connect', {ssid:'Lab', password:'hunter2'}), {});
  assert.deepEqual(sanitizeParams('/api/unknown', {id:'1'}), {});
  assert.deepEqual(sanitizeParams('/api/move', {id:'1', angle:'90', notes:{nested:'x'}}), {id:'1', angle:'90'});
  // Long values are clipped so one field cannot fill the history.
  assert.equal(sanitizeParams('/api/command', {hex:'A'.repeat(400)}).hex.length, 160);

  // Response bodies keep only named primitives.
  assert.deepEqual(summarizeBody({ok:false, error:'not_enabled', line:12, token:'x', limits:{a:1}}), {ok:false, error:'not_enabled', line:12});
  assert.deepEqual(summarizeBody('not an object'), {});

  // Nested objects lose credential-looking keys at any depth.
  const redacted = redactObject({ssid:'Lab WiFi', saved:true, nested:{password:'hunter2', ip:'192.168.1.50'}, token:'secret'});
  assert.deepEqual(redacted, {saved:true, nested:{ip:'192.168.1.50'}});
  assert.doesNotMatch(JSON.stringify(redacted), /hunter2|Lab WiFi|secret/);
});

test('the submitted queue program is kept as text, bounded and flagged', () => {
  const program = 'move 1 90\nwait 200\nhome 2 2';
  const safe = sanitizeParams('/api/queue/start', {program, repeat:'2'});
  assert.equal(safe.program, program, 'the DSL text is preserved exactly as sent');
  assert.equal(safe.programTruncated, false);
  assert.equal(safe.repeat, '2');
  assert.equal(sanitizeParams('/api/queue/start', {program, password:'x'}).password, undefined);

  const huge = 'A'.repeat(PROGRAM_TEXT_MAX + 500);
  const clippedProgram = sanitizeParams('/api/queue/start', {program:huge});
  assert.equal(clippedProgram.program.length, PROGRAM_TEXT_MAX);
  assert.equal(clippedProgram.programTruncated, true, 'truncation is explicit, not silent');

  const { store, storage, runTimers } = makeStore();
  const requestId = store.beginRequest({method:'POST', path:'/api/queue/start', params:{program, repeat:'1'}});
  const submitted = store.snapshot().events[0];
  assert.equal(submitted.requestId, requestId);
  assert.equal(submitted.program, program);
  assert.match(submitted.detail, /program=3 行 \/ 27 字符/);
  assert.equal(programSummary(program), '3 行 / 27 字符');
  assert.match(submitted.detail, /提交不等于执行/);

  // Wi-Fi credentials and unknown fields never appear, whatever is posted.
  store.beginRequest({method:'POST', path:'/api/wifi/connect', params:{ssid:'Lab', password:'hunter2'}});
  store.beginRequest({method:'POST', path:'/api/enable', params:{id:'1', enabled:'1', secret:'x'}});
  const events = store.snapshot().events;
  assert.equal(events.length, 3);
  assert.match(events[1].detail, /POST \/api\/wifi\/connect · 已提交/);
  assert.doesNotMatch(events[1].detail, /Lab|hunter2/);
  assert.match(events[2].detail, /id=1 enabled=1/);
  assert.doesNotMatch(events[2].detail, /secret/);

  // A long program is persisted only for the newest submissions.
  for (let i = 0; i < PERSISTED_PROGRAMS + 3; ++i) {
    store.beginRequest({method:'POST', path:'/api/queue/start', params:{program:`move 1 ${i}`, repeat:'1'}});
  }
  store.flush();
  const restored = readPersisted(storage);
  const withProgram = restored.events.filter((event) => event.program);
  assert.equal(withProgram.length, PERSISTED_PROGRAMS);
  assert.equal(withProgram.at(-1).program, `move 1 ${PERSISTED_PROGRAMS + 2}`);

  // The export carries the text with its event.
  const markdown = exportMarkdown(store.snapshot(), {origin:'http://device.test'});
  assert.match(markdown, /move 1 90/);
  assert.match(markdown, /wait 200/);
  runTimers();
});

test('a POST produces one submitted/result pair and never leaks the body', () => {
  const { store } = makeStore();
  const requestId = store.beginRequest({method:'POST', path:'/api/command', params:{hex:'0145660000786B'}});
  assert.match(requestId, /^[a-z0-9]+-r1$/);
  store.finishRequest(requestId, {path:'/api/command', status:202, body:{ok:true, message:'queued', id:1}, durationMs:41});
  store.finishRequest(store.beginRequest({method:'POST', path:'/api/move', params:{id:'1', angle:'90'}}),
    {path:'/api/move', status:409, body:{ok:false, error:'not_enabled', line:0}, durationMs:12});
  store.failRequest(store.beginRequest({method:'POST', path:'/api/stop', params:{id:'1'}}),
    {path:'/api/stop', status:0, error:'request_timeout', uncertain:true, durationMs:1801});

  const events = store.snapshot().events;
  assert.deepEqual(events.map((e) => e.event), ['request.submitted', 'http.result', 'request.submitted', 'http.result', 'request.submitted', 'request.error']);
  assert.match(events[0].detail, /hex=0145660000786B/);
  assert.match(events[1].detail, /HTTP 202/);
  assert.match(events[1].detail, /已入队（202，仅代表板端已接受/);
  assert.match(events[1].detail, /message=queued/);
  assert.match(events[3].detail, /HTTP 409/);
  assert.match(events[3].detail, /error=not_enabled/);
  assert.equal(events[5].uncertain, true);
  assert.match(events[5].detail, /不会自动重发/);
  // A failed request keeps its request id so the pair is traceable.
  assert.equal(events[4].requestId, events[5].requestId);
  assert.equal(store.beginRequest({method:'GET', path:'/api/status'}), null, 'GETs never create a submitted event');
});

test('GET polling logs failures once, records changes, and ignores noise', () => {
  const { store } = makeStore();
  // A failure is logged once; the repeats and the recovery are one line each.
  store.noteGet({path:'/api/limits', ok:false, status:503, error:''});
  store.noteGet({path:'/api/limits', ok:false, status:503, error:''});
  store.noteGet({path:'/api/limits', ok:false, status:503, error:''});
  store.noteGet({path:'/api/limits', ok:true});
  assert.deepEqual(store.snapshot().events.map((e) => e.event), ['http.failed', 'http.recovered']);

  // Status changes are recorded for state fields only, never for a moving value.
  store.noteStatus({id:1, state:'disabled', fault:'none', enabled:false, canReady:true, lastAck:'none', positionDeg:1});
  store.noteStatus({id:1, state:'disabled', fault:'none', enabled:false, canReady:true, lastAck:'none', positionDeg:2});
  store.noteStatus({id:1, state:'idle', fault:'none', enabled:true, canReady:true, lastAck:'received', positionDeg:2});
  const afterStatus = store.snapshot().events.filter((e) => e.event === 'status.changed');
  assert.equal(afterStatus.length, 1);
  assert.match(afterStatus[0].detail, /state: idle/);
  assert.doesNotMatch(afterStatus[0].detail, /positionDeg/);

  // Queue and limits behave the same way, and a malformed payload is ignored.
  store.noteQueue({state:'running', runId:3, step:1, message:'running'});
  store.noteQueue({state:'running', runId:3, step:2, message:'running'});
  store.noteQueue({garbage:true});
  assert.equal(store.snapshot().events.filter((e) => e.event === 'queue.changed').length, 1);
  assert.deepEqual(limitsKey({maxCurrentMa:500, maxSpeedRpm:60, noise:1}), {maxSpeedRpm:60, maxCurrentMa:500});
  assert.deepEqual(queueKey({state:'idle'}), {state:'idle'});
  assert.deepEqual(statusKey({positionDeg:5}), {});
  assert.equal(formatChanges({state:'idle'}), 'state: idle');
});

test('the periodic query rotation never floods the CAN history', () => {
  // Poll replies and the board's own probes are periodic traffic.
  assert.equal(isPeriodicQueryFrame({dir:'RX', data:[0x36,0,0,0,0,125,0x6b]}), true);
  assert.equal(isPeriodicQueryFrame({dir:'TX', data:[0x36,0x6b]}), true);
  assert.equal(isPeriodicQueryFrame({dir:'RX', data:[0x3a,0x01,0x6b]}), true);
  // Control frames - including the ones this page submitted - are kept.
  assert.equal(isPeriodicQueryFrame({dir:'RX', data:[0xf3,0x02,0x6b]}), false);
  assert.equal(isPeriodicQueryFrame({dir:'RX', data:[0x45,0x02,0x6b]}), false);
  assert.equal(isPeriodicQueryFrame({dir:'TX', data:[0x45,0x66,0,0,0x78,0x6b]}), false);
  assert.equal(isPeriodicQueryFrame({dir:'TX', data:[0xfd,1,0x0f,0xa0,0,0,1,0xfa]}), false);

  const { store } = makeStore();
  const added = store.recordTraceRows([
    {dir:'RX', canId:0x100, data:[0x36,0,0,0,0,125,0x6b], at:1, time:'t1'},
    {dir:'TX', canId:0x100, data:[0x36,0x6b], at:2, time:'t2'},
    {dir:'RX', canId:0x100, data:[0xf3,0x02,0x6b], at:3, time:'t3'},
  ]);
  assert.equal(added, 1);
  const snapshot = store.snapshot();
  assert.equal(snapshot.trace.length, 1);
  assert.match(snapshot.trace[0].text, /RX id=0x100 F3 02 6B/);
});

test('the board log dedups by boot and seq, and reports restart and gaps', () => {
  const first = boardPayload('AAAA', 3, [
    {seq:1, atMs:10, level:'info', event:'boot', detail:'reset=1'},
    {seq:2, atMs:20, level:'info', event:'can.init', detail:'ready=1'},
    {seq:3, atMs:30, level:'warn', event:'http.result', detail:'/api/command code=409'},
  ]);
  const parsed = parseBoardLog(first);
  assert.equal(parsed.ok, true);
  assert.deepEqual(boardDelta(parsed, {bootId:'', seq:0}), {restarted:true, fresh:parsed.events, gap:null, bounded:false, omitted:0, sequence:3, bootId:'AAAA'});

  // Same payload again: nothing new is imported.
  assert.equal(boardDelta(parsed, {bootId:'AAAA', seq:3}).fresh.length, 0);
  // One new event is imported exactly once.
  const second = parseBoardLog(boardPayload('AAAA', 4, [...first.events, {seq:4, atMs:40, level:'info', event:'queue.state', detail:'run=1 state=1'}]));
  const delta = boardDelta(second, {bootId:'AAAA', seq:3});
  assert.deepEqual(delta.fresh.map((e) => e.seq), [4]);
  // The ring moved past what this browser had seen: that is a gap, not silence.
  const jumped = parseBoardLog(boardPayload('AAAA', 90, [{seq:80, atMs:1, level:'info', event:'x', detail:''}]));
  assert.deepEqual(boardDelta(jumped, {bootId:'AAAA', seq:3}).gap, {from:4, to:79});
  // A new bootId means the RAM ring was lost with the power.
  assert.equal(boardDelta(parsed, {bootId:'BBBB', seq:9}).restarted, true);

  const { store } = makeStore();
  assert.deepEqual(store.mergeBoardLog(first).imported, 3);
  assert.equal(store.mergeBoardLog(first).imported, 0, 'the same ring is not imported twice');
  const withGap = store.mergeBoardLog(boardPayload('AAAA', 95, [{seq:90, atMs:5, level:'info', event:'later', detail:'d'}]));
  assert.deepEqual(withGap.gap, {from:4, to:89});
  const restart = store.mergeBoardLog(boardPayload('CCCC', 1, [{seq:1, atMs:5, level:'info', event:'boot', detail:'reset=3'}]));
  assert.equal(restart.restarted, true);
  const events = store.snapshot().events;
  assert.ok(events.some((e) => e.event === 'board.gap'));
  assert.ok(events.some((e) => e.event === 'board.restart'));
  assert.ok(store.snapshot().gaps.length >= 2);
  // Board events keep the board's own uptime, never the browser's clock claim.
  const bootEvent = events.find((e) => e.event === 'boot');
  assert.equal(bootEvent.boardMs, 10);
  assert.equal(store.snapshot().bootId, 'CCCC');
});

test('a bounded board response is not mistaken for a ring overwrite', () => {
  const payload = parseBoardLog({
    bootId:'AAAA', sequence:60, capacity:48, omitted:20,
    events:[{seq:41, atMs:1, level:'info', event:'boot', detail:''}],
  });
  assert.equal(payload.omitted, 20);
  const delta = boardDelta(payload, {bootId:'AAAA', seq:3});
  assert.equal(delta.gap, null, 'nothing was overwritten, the response was bounded');
  assert.equal(delta.bounded, true);
  assert.deepEqual(delta.fresh.map((event) => event.seq), [41]);

  const { store } = makeStore();
  store.mergeBoardLog(boardPayload('AAAA', 3, [{seq:1, atMs:1, level:'info', event:'boot', detail:''}]));
  const merged = store.mergeBoardLog(payload);
  assert.equal(merged.gap, null);
  const events = store.snapshot().events;
  assert.ok(events.some((event) => event.event === 'board.bounded'));
  assert.equal(events.some((event) => event.event === 'board.gap'), false);
  assert.equal(store.snapshot().gaps.length, 0);
  // The same bounded count is not reported again on every poll.
  store.mergeBoardLog({...payload, sequence:61, events:[{seq:61, atMs:2, level:'info', event:'x', detail:''}]});
  assert.equal(store.snapshot().events.filter((event) => event.event === 'board.bounded').length, 1);
});

test('control blockers are part of the status key without their ages', () => {
  const withBlockers = (ageMs, blockers) => ({
    id:1, state:'moving', fault:'none', canReady:true,
    control:{busy:true, stationary:false, fault:'none', faultId:0, faultGlobal:false, blockers, blockerCount:blockers.length},
  });
  const first = statusKey(withBlockers(100, [{id:2, reason:'move_active', ageMs:100}]));
  assert.equal(first.blockers, '2:move_active');
  assert.equal(first.controlBusy, true);
  assert.equal(first.blockerCount, 1);
  // The age ticking up is not a change: an idle board must not fill the log.
  const later = statusKey(withBlockers(9999, [{id:2, reason:'move_active', ageMs:9999}]));
  assert.deepEqual(diffFields(first, later), {});
  // A blocker appearing or disappearing is.
  const cleared = statusKey(withBlockers(0, []));
  assert.deepEqual(diffFields(first, cleared), {blockers:'', blockerCount:0});
});

test('the board endpoint missing or unreadable is reported once, never as offline', () => {
  const { store } = makeStore();
  store.noteBoardLogResult({ok:false, status:404, error:'not found'});
  store.noteBoardLogResult({ok:false, status:404, error:'not found'});
  assert.equal(store.snapshot().events.filter((e) => e.event === 'board.unavailable').length, 1);
  assert.equal(store.snapshot().boardAvailable, false);
  assert.match(store.snapshot().events[0].detail, /固件未更新/);
  store.noteBoardLogResult({ok:true});
  assert.equal(store.snapshot().boardAvailable, true);
  assert.equal(store.snapshot().events.filter((e) => e.event === 'board.recovered').length, 1);
  // A rejected payload is not accepted as history.
  assert.equal(store.mergeBoardLog({bootId:'', events:[]}).accepted, false);
  assert.equal(store.mergeBoardLog('nope').accepted, false);
});

test('exports carry evidence, escape text and drop credentials', () => {
  const { store } = makeStore();
  store.add({source:'session', event:'boot', detail:'quote " backslash \\ newline \n end'});
  store.recordTraceRows([{dir:'RX', data:[0xf3,0x02,0x6b], at:1, time:'10:00:00.000'}]);
  store.noteLimits({maxSpeedRpm:120, maxAccelRpmS:240, maxCurrentMa:5000, maxAngleDeg:3600, maxMoveSeconds:60, experimentSeconds:5});
  const snapshot = store.snapshot();

  const context = {
    origin:'http://device.test', exportedAt:'2026-09-22T10:00:00.000Z',
    limits:{maxCurrentMa:5000, password:'hunter2'},
    motor:{id:2, state:'idle', fault:'none', ssid:'Lab WiFi'},
    queue:{state:'running', runId:4},
    config:{state:'config_verified', sequence:2},
  };
  const md = exportMarkdown(snapshot, context);
  assert.match(md, /来源: http:\/\/device\.test/);
  assert.match(md, /maxCurrentMa/);
  assert.match(md, /"state":"running"/);
  assert.match(md, /config_verified/);
  assert.match(md, /RAM 环形缓冲/);
  assert.match(md, /F3 02 6B/);
  assert.match(md, /quote " backslash \\ newline/);

  const json = exportJson(snapshot, context);
  const parsed = JSON.parse(json);
  assert.equal(parsed.logVersion, LOG_VERSION);
  assert.equal(parsed.motor.state, 'idle');
  assert.equal(parsed.events.length, snapshot.events.length);
  assert.equal(parsed.trace.length, 1);
  // No credential may survive into any export, at any depth.
  for (const text of [md, json]) {
    assert.doesNotMatch(text, /hunter2/);
    assert.doesNotMatch(text, /Lab WiFi/);
    assert.doesNotMatch(text, /ssid/);
    assert.doesNotMatch(text, /password/i);
  }
});

test('history survives a reload; clearing keeps the board cursor', () => {
  const storage = memoryStorage();
  const first = makeStore({ storage });
  first.store.add({source:'session', event:'before-reload', detail:'kept'});
  first.store.mergeBoardLog(boardPayload('DDDD', 2, [{seq:1, atMs:1, level:'info', event:'boot', detail:''}, {seq:2, atMs:2, level:'info', event:'can.init', detail:''}]));
  assert.equal(first.store.flush(), true);

  const second = makeStore({ storage });
  const restored = second.store.snapshot();
  assert.equal(restored.events.some((e) => e.event === 'before-reload'), true);
  assert.equal(restored.bootId, 'DDDD');
  assert.deepEqual(restored.cursor, {bootId:'DDDD', seq:2});
  assert.equal(second.store.beginRequest({method:'GET', path:'/api/status'}), null);

  // Clearing the local history does not re-import the ring that is still there.
  second.store.clear();
  assert.equal(second.store.snapshot().events.length, 1, 'only the clear marker remains');
  assert.deepEqual(second.store.snapshot().cursor, {bootId:'DDDD', seq:2});
  assert.equal(second.store.mergeBoardLog(boardPayload('DDDD', 2, [{seq:1, atMs:1, level:'info', event:'boot', detail:''}])).imported, 0);
  // A genuinely new board event after the clear is still imported.
  assert.equal(second.store.mergeBoardLog(boardPayload('DDDD', 3, [{seq:3, atMs:9, level:'warn', event:'motor.fault', detail:'fault=move_timeout'}])).imported, 1);
});

test('corrupt or denied storage and a throwing subscriber never break the caller', () => {
  for (const payload of ['', '{not json', '[]', 'null', JSON.stringify({version:99, events:[{a:1}]})]) {
    const restored = readPersisted(memoryStorage(payload));
    assert.deepEqual(restored.events, []);
    assert.deepEqual(restored.cursor, {bootId:'', seq:0});
  }
  const denied = createLogStore({storage: deniedStorage(), wallClock: () => 1, setTimer: () => 1, clearTimer: () => {}});
  denied.add({source:'session', event:'works', detail:'in memory'});
  assert.equal(denied.snapshot().events.length, 1);
  assert.equal(denied.flush(), false, 'persistence failure is reported, never thrown');

  // A subscriber that throws is isolated: the request path keeps working.
  const { store } = makeStore();
  const unsubscribe = store.subscribe(() => { throw new Error('subscriber exploded'); });
  assert.doesNotThrow(() => store.beginRequest({method:'POST', path:'/api/stop', params:{id:'1'}}));
  assert.doesNotThrow(() => store.finishRequest('r1', {path:'/api/stop', status:202, body:{}, durationMs:5}));
  assert.equal(store.snapshot().events.length, 2);
  unsubscribe();
  assert.equal(writePersisted(null, emptyState()), false);
  assert.equal(readPersisted(null).version, LOG_VERSION);
  // Only the log key is ever touched: motor drafts and the queue program keep
  // their own storage.
  const { store: scoped, storage } = makeStore();
  scoped.add({source:'session', event:'x', detail:''});
  scoped.flush();
  assert.deepEqual([...storage.map.keys()], [LOG_STORAGE_KEY]);
});

test('the debounced save is bounded and explicit flush is immediate', () => {
  const { store, storage, runTimers } = makeStore();
  for (let i = 0; i < 30; ++i) store.add({source:'session', event:`e${i}`, detail:''});
  assert.equal(storage.getItem(LOG_STORAGE_KEY), null, 'nothing is written before the debounce fires');
  runTimers();
  assert.notEqual(storage.getItem(LOG_STORAGE_KEY), null);
  assert.equal(store.snapshot().events.length, 30);
  assert.equal(SAVE_DEBOUNCE_MS > 0, true);
  assert.equal(MAX_TRACE_FRAMES, 400);
});
