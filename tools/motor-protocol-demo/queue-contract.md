# Board queue contract (2026-09-22)

Desktop device page only. Board executes queue independently of browser; no auto-start after reboot, no retries on uncertain submission. Program text saved locally only until explicit start. Queue in board RAM, rotation distances in NVS. Whole program validated before any CAN TX.

POST /api/queue/start form {program, repeat} (repeat integer 1..1000). Max 64 nonblank actions, max 8192 text bytes. GET /api/queue => {state:idle|running|done|failed|cancelled, runId:number, step:number (1-based,0 before start), total:number, iteration:number (1-based), repeat:number, line:number (source line), action:string, message:string, raw:boolean}. POST /api/queue/cancel empty form cancels and stops all; global stop also cancels queue. Running state survives browser disconnect, never board reboot. Busy start returns409, invalid program400 with error and line. POST start returns202 with same status shape. GET available without executing anything.

GET /api/motor-distance?id=1 => {id:1, rotationDistance:null|positive number} in mm/rev. POST same path {id,rotationDistance} saves positive finite0.000001..1000000, or0 clears; returns confirmed object. Save refused while queue/other operation active. Never guess defaults. Each ID1..255 independent. Convert mm to degrees on board using snapshot at validation; no editing during run.

DSL one action per line, '#' comment, case-insensitive command and unit. Whitespace separated numbers strictly parsed, no expressions/eval. Short form defaults explicitly shown in editor. Optional args trailing only. Signed distance/current/RPM controls direction. Board validates against current DebugLimits, rejects overflow and zero-rounding.

- enable ID — waits actual F3 ACK. No implicit enable; examples include enable lines, builder can insert once per target when explicit checkbox selected.
- disable ID — waits ACK and confirmed stationary.
- move ID VALUE [deg|rev|mm] [RPM [ACCEL [DECEL [CURRENT]]]] — defaults unit deg, RPM30, accel60, decel60,current800mA. Waits actual supervised CD completion. rev=360deg. mm per-ID rotation distance. Defaults don't silently clamp to policy.
- home ID [MODE] — mode0..5 default0; waits supervised9A outcome. Mode labels as manual. "复位" means home here, not MCU reboot/current-position-zero.
- torque ID SIGNED_MA DURATION_MS [MAX_RPM [RAMP_MA_S]] — default maxRPM30,ramp1000. C5 limited-speed torque, duration integer1..3600000, stop at elapsed time then wait stationary. Policy experimentSeconds if nonzero also enforced: reject longer requested duration instead of silently stopping early. No Nm claim.
- velocity ID SIGNED_RPM DURATION_MS [ACCEL [CURRENT]] — defaults60,800; same bounded duration/stop behavior C6.
- stop ID — wait stationary (preserves enable).
- wait MS — integer0..3600000, nonblocking.
- hex AA BB ... — 3..30 byte logical X command (includes address/checksum), transmitted without opcode whitelist, preserves every byte, driver CAN fragmentation; no completion inferred. Allows sync/unknown functions; no software parameter policy applied. Reports sent only. No implicit stop after raw step.
- can ext|std IDHEX BYTE... — single actual CAN data frame, ID is hex with optional0x,0..1FFFFFFF ext or0..7FF std; 0..8 bytes. Exact bytes, no rewrite/6B addition. Reports sent only. Between frames at least2ms. No automatic retry at application level. Actual TX trace.

Raw paths only check framing/CAN bounds; user intentionally gets direct bus control. Running raw queue UI plainly shows no device-controller supervision for raw steps. Explicit cancellation/stop broadcasts9C+FE (abort raw homing too); no fabricated stationary success. Raw sends invalidate prior software enable/position completion ownership, don't invent motor effects. Raw queue finish means all frames submitted, not motors stopped. Structured steps require proper explicit enable after raw if necessary.

Runtime fail-fast: first rejection/fault/timeout stops/cancels remaining actions and requests stop. Step watchdog configurable maxMoveSeconds for supervised steps (controller home/move handles own bounds); enable/stop bounded. No stale previous ACK/outcome completes new step. HTTP manual mutations and UART operations cannot interleave with active queue; stop/disable/abort remain accessible and cancel queue first. Reads remain accessible. Queue start while UART/wifi/controller busy rejected; Wi-Fi/config/limits blocked during queue. UART STOP cancels queue before future steps.

UI: new 编排队列 tab; compact readable source editor + action insertion form + live board progress, repeat, validate/preview, explicit start/cancel, load sample, copy/export/import local text. Show source line errors. Per-ID rotation-distance editor. Show semantic preview converted mm angle using confirmed per-ID config; server authoritative. Reconnect reads status only. User examples should need few lines, not JSON plumbing. Start only with valid input and board connected, locks synchronously to prevent double-click. Avoid optimistic done. No mobile work.
