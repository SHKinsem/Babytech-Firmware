# Retain motor form drafts across navigation

Implement focused frontend change. User: 切换走之后还能保存原来刚刚设置的参数. Main pain: DeviceApp.choose and variant changes reset deviceDefaults. Preserve existing work. No shell, hardware, flash, commit; Codex validates. Use same Claude session.

Own tools/motor-protocol-demo/src/DeviceApp.jsx, new src/device-drafts.js helper if useful, tests/device-drafts.test.mjs, qa-drafts.mjs and append npm test script. No firmware behavior or WiFi/scale/limits persistence changes. Add minimal concise visible note that inputs are local drafts, not confirmed board values. No visual redesign or mobile work.

Requirements:
- Save/restore lab forms by motor ID + command ID + variant key, so A->B->A and FB->CB->FB restore edits without sharing unrelated motor settings. Switching motorID should stash previous then restore correct drafts; invalid intermediate ID inputs must not overwrite valid motor drafts.
- Persist to versioned browser localStorage so reload/reopen same origin also retains. Never persist passwords, device enabled/online/limits/ACK or execution state. Keep queued program's existing storage untouched.
- Preserve exact user input strings including partial/invalid values. Defaults only for never-edited/new forms; changed board limits validate old drafts, never silently clamp them.
- Preserve raw HEX draft and form/raw mode per same key, but never automatically send. Returning to form must not destroy unsent raw edits; explicit reset-to-form remains available via existing onResetRaw. If raw frame targets differentID existing gates must stillblock.
- Keep last selected variant per command to return to what user was editing. Persist command selection and valid motorID if useful, but no automatic POST on restore. Tabs can startmanual as before. Restore manualtrial fields per motor too, no autoexecute.
- Defensive localStorage read/write exceptions/corrupt schema/unknowncommand orvariant -> safe defaults. Bound storeddata and sanitize known editable keys with string/number/bool primitives only; no need generalstorageframework. Avoid useEffect save-first overwriting restored drafts; explicit currentcontext ref or atomic state transition.
- Existing limits and queue/API poll must not overwrite edited params.

Tests: purehelper roundtrip/corruptstorage/nosensitivefields; Playwright mockdevice navigate homingparams edits120 andmode2, tocurrentlimit200, back preserves; variants independentlysaved; ID1vs2 isolated including invalid transientID; tabaway+back; reloadretainsvalues and raw; freshload noPOST; localStorage denied/corrupt doesn'tcrash; boardlimitsstillgate restoredoutofrange. Use exact#field-* selectors not fuzzylabels. Reuse QAfixture minimalno unrelatedtests. Reportchangedpaths briefly whencomplete; avoid longselfreview.
