# Finish logging + simple clear-state button

Continue same session. User latest explicitly says “给我一个清除板端状态的按钮就行”. NO automatic re-enable/recovery flow. Keep requested logs, add one direct operator button. Other writer Codex has now completed isolated controller fixes; do not overwrite them. Read current source.

Codex-added backend contract already in workspace:
- CommandQueue::clearControlState(): cancels queue, sends abort/stop via existing cancel once, resets queue to idle and clears MotorControl volatile ownership regardless stop TX result. Returns result of stop attempt. No auto-enable/move, no NVS/CAN reinit, retains diagnostic evidence.
- Endpoint::cancelPending(Frame& event): finishes one pending UART exec/stop as Cancelled, preserving dedup/cached results; call until false and sendFrame(event). No backend stop itself.
- MotorControl statusJson now has control:{busy,stationary,fault,faultId,faultGlobal,blockers:[{id,reason,ageMs}],blockerCount}. Upper16 displayed; total count full. Do not change policy or this contract. Codex owns tests_motor_control + tests_queue_uart and controller/shared files.
- Fixed broadcast stop inventing pending stop for never-seen UI selection. Do not revert. Regression added by Codex.

Your ownership now: previous logging files, main.cpp, new frontend button/test files. Do NOT touch MotorControl/CommandQueue/BoardEndpoint or their tests.

## Immediate known fixes in your logging implementation
1. Native compile failed DebugLog.h: `const Entry& at(...)` appears before Entry declaration. Move public Entry definition before usage. kJsonMax currently ignores worst-case JSON escaping (up to6x strings), so valid ring can return empty; instead reserve correct bounded max or safe serialization design. Fixed global buffer is fine within RAM but calculate size. addf truncation must flag if vsnprintf return exceeds buffer, not silently report false. Tests should fill ring with escaped controls and validate writeJson with kJsonMax, addf truncation flag.
2. request logger currently omits queue program entirely. Log the submitted DSL text bounded at8192 with explicit truncation if necessary so user can diagnose which instruction they sent. Do not log WiFi credentials or arbitrary request args. `enabled` actual parameter key check current endpoint/page; log correct key. Preserve data values as sent.
3. Review and run-dependent findings forthcoming; file-only worker. Codex will run npm/native/browser after you finish.

## New API and button
- main POST `/api/control/reset`, available while queue/device-controller/config/stop/fault busy, NOT behind existing motionBusy gate. Handler first capture/log current status/queue diagnostic summary, then terminate UART ownership via cancelPending/sendFrame (bounded2), queue.clearControlState once. Response200 {ok:true,stateCleared:true,stopSent:true,message:'control_state_cleared'} if stop attempt code<300; if failure HTTP503 {ok:false,stateCleared:true,stopSent:false,error:'control_state_cleared_stop_unconfirmed'}; physicalStopped MUST NOT true/implied. Clearing internal state is not proof shaft stopped. No reset NVS/WiFi/limits/rotation calibration/drafts/log history; no auto resend/enable/move/reboot. Include endpoint in known paths and log allowlist; log reset event. GET cannot mutate.
- Single visible toolbar button “清除板端状态” near stop, usable on ALL tabs and when queue/motor busy. No modal/confirmation. Small tooltip explains cancels queue, attempts stop, clears volatile busy/fault/ACK state, keeps config, must explicitly enable later. Handle no-auto-retry semantics: timeout/network uncertain show unknown and DO NOT locally claim cleared; 200 or503 payload stateCleared=true clear stale local ownership via epochs/refetch (preserve no fake hardware confirmation). Queue mutation epoch prevents inflight poll overwriting reset; busy prior operations must not auto-submit after reset. Add request generation if necessary so late prior submit responses don't overwrite reset notice. On old firmware404 say button requiresnewfirmware. Prevent doubleclick while resetpending but stopstillusable. Do not make this a re-enable flow.
- Display control blockers from fresh current status somewhere compact in feedback/log tab (motorID+reason, not age changes logged everypoll). Add translated reset errors. Retain logs acrossreset.
- Browser test script qa-debug-log.mjs is required (did you create?); cover current logs +clear-state whilebusy, exactlyone resetPOST and zeroenable/move, success/503clearedstopfailed/timeoutsunknown/404, late queuepoll afterreset, logsnoterased. Desktop1513x1039 +1280x800 (toolbar should fit). Author tests, Codexruns.

Please finish compactly. Return concrete changes and limitations; no invented tests.
