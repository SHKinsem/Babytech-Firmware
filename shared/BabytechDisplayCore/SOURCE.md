# Display protocol source

Vendored from Babytech_Formula_Device, branch V1-device, repository commit
40cfcde (DisplayCore last changed in 2337b2fad83060cf74c7c5650001eb3f1b97872c).
Protocol version 3, snapshot schema 3. Model/protocol sources and their host
tests were initially copied unchanged. Generated UI strings and LVGL are not needed here.

2026-09-23 additive update, kept identical with the product repository at 62abe5d:
BabytechDisplayCore: Initialize=2, intent validation and initialization button
eligibility tests. Protocol/schema remain v3; no State field or stage changes.

Motion constructs DisplaySnapshot directly. buildDisplaySnapshot() is the
product's cloud/context policy and must not be used for the standalone demo.
Keep wire enum values unchanged even when the demo emits only a subset.
