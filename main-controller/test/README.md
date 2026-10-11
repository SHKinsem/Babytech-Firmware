Brain uses the migrated screen and UART v3 baseline. From the firmware root,
build `pio run -d main-controller -e brain`, then run
`python tools/test_display_view.py` and `python tools/test_demo.py`.
The latter covers the shared v3 codec and Motion demo behavior; neither test
replaces physical touch/UART validation. `tools/test_protocol.py` tests the older
v2 protocol library, not Brain's current screen client.
Network/product migration and hardware acceptance remain in progress.
