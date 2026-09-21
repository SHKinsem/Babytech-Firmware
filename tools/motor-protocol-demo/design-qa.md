# Design QA — Babytech motor protocol demo

- Source visual truth: `references/design.png` (selected protocol workbench design).
- Implementation: `http://127.0.0.1:4175/`.
- Browser: local headless Chrome through Playwright, explicitly authorized by the user after in-app browser automation was unavailable.
- Main viewport: 1513 × 1039 CSS px, deviceScaleFactor 1. Source and screenshot both 1513 × 1039 pixels; no resizing before comparison.
- Main state: instruction lab, F3 enable command, motor 01, seeded simulated feedback.
- Implementation screenshot: `qa/desktop.png`.
- Full-view evidence: `qa/comparison.png`, reference and implementation in one side-by-side image.
- Focused evidence: `qa/comparison-detail.png`, parameter controls, logical command, CAN payload and actions.
- Laptop evidence: `qa/desktop-1280.png` and `qa/desktop-1280-scrolled.png` at 1280 × 800, density 1.
- Additional state evidence: `qa/position-command.png`, `qa/manual.png`.

## Comparison history

1. Initial runtime: P0 empty screen caused by an unimported RPM scale constant. Fixed by Claude Code, then verified through a real browser render.
2. Initial layout: P2 undersized typography and uneven column proportions; P2 send controls below the visible region on 1280 × 800. Increased body/control typography, matched the three-column proportions to the reference, restored dark brand text and the subtle feedback surface. Made the command action row sticky. Recaptured both desktop viewports and verified the preview remains reachable by internal scrolling.
3. Functional QA: fixed incorrect unit-test expected duration/length, raw command validation, broadcast stop, cancellation after motor changes, unmodelled response boundaries and string-valued variant controls through Claude Code. Final acceptance additionally fixed a missing copy-frame import and ensured a synchronized command leaves motor readouts unchanged until FF triggers it.
4. Post-fix comparison: full-view and focused side-by-side evidence reviewed. No remaining actionable P0/P1/P2 visual or core-interaction defects found.

## Required fidelity surfaces

- Typography: system Chinese UI sans-serif, 15px primary labels and controls, 22px section headings, tabular/monospaced frame bytes. Smaller secondary metadata remains subordinate; main labels and primary actions readable at both desktop widths.
- Spacing/layout: preserved toolbar, two mode tabs, command library, editor, feedback inspector and trace drawer. Independent panel scroll is intentional for long parameter forms and shorter screens. Stop and send actions remain reachable. No horizontal document overflow at 1280px.
- Colors/tokens: white/indigo interface, thin blue-gray separators, pale blue-gray feedback background, green status and red stop. Flat buttons intentionally replace the image generator's decorative gradients.
- Assets/icons: reference has no photographic or illustrative assets. Wordmark is text. Icons use Phosphor; no custom drawn icon placeholders.
- Copy/content: Chinese terminology and exact F3 example match source intent. Added an explicit simulation label and corrected capability badges to distinguish basic firmware interfaces from additional driver commands. Timestamps and seeded trace labels are demo data, not hardware readings.

## Validation

- `npm test`: 21/21 protocol and planner tests passed.
- `npm run build`: passed after final changes.
- `qa/smoke-results.json`: 9 browser checks passed, no uncaught page errors.
- `qa/interaction-results.json`: 7 additional interaction checks passed, no uncaught page errors.
- Covered: F3 send/ACK, CD multipacket preview, invalid HEX/checksum/known fields, motor ID validation, independent motor state, no delayed motion after stop or switch, copy actual frames, TX/RX filters, pause/resume trace, address mismatch, unknown TX-only commands, torque queued until FF, and actual ID0 stop broadcast without fabricated RX.
- Real hardware connections, firmware flashing and physical response validation are outside this local demo. Only source-defined response layouts are simulated.

## Follow-up polish

- P3: native system font metrics and compact secondary metadata differ slightly from generated typography; intentional for an offline desktop demo.
- P3: at shorter laptop heights, detailed frames and expanded categories use panel scrolling instead of reducing text size.

final result: passed

## Device integration QA — 2026-09-21

The selected desktop layout is retained. `DeviceApp.jsx` replaces simulated transport and feedback with device APIs and adds Wi-Fi as a third tab. Inspect `qa/device-lab-1513.png`, `qa/device-wifi-1513.png`, and `qa/device-manual-1280.png` for the final embedded build. Browser test fixtures use simulated HTTP responses; the shipped page has no simulated telemetry.

Desktop-only acceptance (per latest user direction): 1513×1039 and 1280×800. No further mobile work is in scope. The Wi-Fi panel follows the existing typography, separators and indigo actions. Shorter desktop screens use internal scrolling for detailed parameters and feedback.

Twelve browser acceptance groups cover protocol requests, enable gating, command/address validation, stop while another request is pending, stale display clearing, Wi-Fi scan/connect/forget, no startup POST and no external assets. Firmware builds independently with the generated HTML embedded. Physical CAN/radio verification was not performed; the user explicitly requested no flashing.

Device desktop design result: passed. Hardware acceptance: not performed.
