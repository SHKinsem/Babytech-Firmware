# Display protocol source

Vendored from Babytech_Formula_Device, branch V1-device, repository commit
40cfcde (DisplayCore last changed in 2337b2fad83060cf74c7c5650001eb3f1b97872c).
Protocol version 3, snapshot schema 3. Model/protocol sources and their host
tests are copied unchanged. Generated UI strings and LVGL are not needed here.

Motion constructs DisplaySnapshot directly. buildDisplaySnapshot() is the
product's cloud/context policy and must not be used for the standalone demo.
Keep wire enum values unchanged even when the demo emits only a subset.
