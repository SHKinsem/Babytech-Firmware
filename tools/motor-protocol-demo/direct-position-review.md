# Review corrections before final verification

Continue same session. No shell. Preserve Codex edits to DeviceApp.jsx (enable gate and direct note), tests/test_raw_can.cpp (real FB/CB transport assertions).

1. Correct a mistake in the original brief: use **0x33 / X42sSysParam::Tpos**, the p70 motor target position, for previous input target and completion target proof. 0x34 / SetTarget is the real-time setpoint (manual p71), which may be an intermediate trajectory value. Inspect p70 vs p71. Rename comments/tests/error text to 33; do not alter the existing generic protocol.js read catalog for 34. Add test that 34 cannot substitute for missing33.
2. serviceQueries currently stops target refresh while a sample is fresh. While a direct job is active, keep querying its target in addition to PV cadence, because completion needs distinct post-start target samples. Only on-demand idle refresh should stop after a fresh sample.
3. directPositionRefusal reads b[1] without ensuring n>=2; guard n before access. Invalid truncated frames must never read outside provided bytes. Ensure directPositionRefusal does not misreport malformed checksum/address as merely unsupported sync.
4. Tests must cover actual directPosition command dispatch (not only pure plan), CB current0 and full modes, incorrect opcode ACK, 33 proof missing/34 wrong, target changed/distant despite current at expected position, cancellation/TX failure/limits. Native compile already passed existing892controller checks before your new tests; Codex will run final checks.

Report what changed; do not edit unrelated existing work.

Native test result from Codex: 1049 checks, 2 failures at tests/test_motor_control.cpp:1421 countTxOpcode(0xFB,2)==1 and :1489 countTxOpcode(0xCB,2)==1. Helper counts physical packets; these commands have two fragments. Inspect and fix those assertions; retain exact-byte packet tests.

JS test result54/56: device-limits.test.mjs:69 asserts VALID CB_EXAMPLE rejected; needs asOpcode(FB_EXAMPLE,0xcb) truncated CB. :113 tests mode2FB_EXAMPLE for prior-target note; change frame to mode0 then expect0x33. Browser qa-direct-position.mjs:64 getByLabel('速度') matches tooltip button and textbox. Use #field-vel, #field-clk, #field-maxCurrentMa or exact textbox labels for inputs. Codex built embedded HTML successfully (413254bytes) before these fixes; rebuild needed afterward.
