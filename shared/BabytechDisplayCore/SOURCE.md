# Display protocol source

Current scope (2026-10-09): Brain/Motion firmware runs UART v4 only. The shared
display model/enums still drive the LVGL UI and Motion status projection;
legacy v3 codecs/tests are internal regression references, not a selectable
firmware transport. All sources required to build are vendored in this child
repository; the original parent DisplayController/libraries are retired.

Vendored from Babytech_Formula_Device, branch V1-device, repository commit
40cfcde (DisplayCore last changed in 2337b2fad83060cf74c7c5650001eb3f1b97872c).
Protocol version 3, snapshot schema 3. Model/protocol sources and their host
tests were initially copied unchanged. Motion uses only the model/protocol.

2026-10-06: the screen baseline moved into main-controller. Brain now uses
shared/BabytechDisplayLvgl and BabytechPanelSt7796, plus the committed
src/generated/feeding_flow_ui.h. The parent repository's
Tools/generate_feeding_flow_ui.py generates that header from its single shared
JSON and records the source SHA-256; --check detects drift. Standalone firmware
builds consume the committed generated header without requiring the parent tree.

2026-09-23 additive update, kept identical with the product repository at 62abe5d:
BabytechDisplayCore: Initialize=2, intent validation and initialization button
eligibility tests. Protocol/schema remain v3; no State field or stage changes.

Motion constructs DisplaySnapshot directly. buildDisplaySnapshot() is the
product's cloud/context policy and must not be used for the standalone demo.
Keep wire enum values unchanged even when the demo emits only a subset.
