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

## Scale drift diagnostics QA — 2026-09-22

- Source visual truth: `C:\Users\xusen\.codex\generated_images\01a0c405-c0b3-7552-b9ce-54f03c98643a\exec-839fb725-b7a3-43ad-bf06-b9a4dd9df9ce.png` (the third displayed ideation result selected by the user).
- Implementation screenshot: `qa/device-scale-1280.png`.
- Browser: Codex in-app browser against the final embedded device build with local fixture responses for `/api/status`, `/api/trace`, and `/api/scale`.
- Viewport and density: implementation 1280 × 800 CSS px at density 1, captured as 1280 × 800 pixels. Source raster is 1672 × 941 pixels; it was reviewed at original density and visually normalized by matching the full desktop frame and section proportions rather than stretching either image.
- State: connected HX711 composite channel, calibrated, stable, live sampling, calibration editor closed.
- Full-view comparison evidence: the source and `qa/device-scale-1280.png` were opened together in one comparison input at original resolution. The header, active tab, three-column proportions, drift chart hierarchy, diagnostic rows, action placement, and bottom event table match the selected direction.
- Focused comparison: no extra crop was required because the original-resolution full views kept chart axes, diagnostic labels, actions, and event columns readable. Accessibility-tree inspection separately confirmed labels, headings, controls, statuses, and table semantics.

### Comparison history

1. First implementation capture used the motor tab after a stale accessibility index. This was a P1 evidence mismatch, not an application defect. The page was reloaded, the current tab index was resolved again, and the selected scale state was recaptured.
2. The initial scale state said “等待板端称重数据” after the first successful sample. This P2 copy/state mismatch was fixed so the first successful poll changes it to “实时采集中”. The embedded page was rebuilt and recaptured.
3. Final comparison found no actionable P0/P1/P2 differences. Remaining data-value differences are expected live telemetry; layout, typography, colors, copy hierarchy, and interaction anatomy match the source.

### Required fidelity surfaces

- Fonts and typography: uses the existing system Chinese UI stack and monospaced numeric stack. Section headings, KPIs, axis labels, diagnostic values, and table copy preserve the source hierarchy without clipping at 1280 × 800.
- Spacing and layout rhythm: preserves the existing 64 px toolbar, 48 px tabs, three-column work area, thin separators, and fixed lower event table. Main controls stay visible at the accepted laptop viewport with no document-level horizontal overflow.
- Colors and tokens: reuses the product's navy text, indigo actions, pale blue-gray side surface, green success, amber drift, red fault, and cool-gray separators. No gradients or decorative elevation were introduced.
- Image quality and assets: the selected screen has no photo or illustration assets. Icons reuse the installed Phosphor package. The live data chart is rendered sharply at device pixel ratio and resizes with its panel.
- Copy and content: identifies HX711 channel 1 as two paralleled full bridges and explicitly states that individual-sensor drift cannot be separated. All displayed diagnostics map to the scale API or browser-derived recent-sample calculations.

### Interaction and runtime validation

- Opened the final embedded build in the in-app browser and tested the scale tab at 1280 × 800.
- Verified live `/api/scale` polling, unique-sample history, 60-second drift chart, noise span, sample-rate estimate, stability duration, filters, search, pause, and clear-record controls.
- Verified `POST /api/scale/tare`, visible in-progress and completed states, and operation records.
- Verified the calibration editor, 500 g submission to `/api/scale/calibrate`, updated `countsPerGram`, success copy, and calibration event row.
- Browser console: no warnings or errors.
- `npm test`: 21/21 protocol and planner tests passed.
- `npm run build:device`: passed; generated one 311,679-byte embedded HTML file with no external runtime assets.
- WSL motion validation: protocol suite 133 checks passed, motor/UART suite 679 checks passed, PlatformIO motion build passed. Final usage: 72,368 bytes RAM (22.1%) and 1,098,297 bytes flash (16.8%).
- Physical HX711 sampling and hardware calibration were not performed in this local visual QA.

### Follow-up polish

- P3: the source mock shows a fully populated 60-second line immediately; the real page intentionally grows the line from the right as actual samples arrive after opening the tab.
- P3: telemetry values and stable/drift color vary with live data, so screenshots will not preserve the mock's exact numbers.

final result: passed
