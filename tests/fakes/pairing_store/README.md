# Pairing Storage Host Tests

Scope: B1.3 pairing storage primitive, not commissioning or runtime activation.

Run from the firmware repository:

```sh
python3 tools/test_pairing_store.py
python3 tools/test_pairing_store.py --sanitize
```

The runner directly links production `BoardPairingStore.cpp`,
`BoardPairingRecord.cpp`, `BoardSessionV4.cpp` and its protocol dependencies.
Only the SDK NVS and MAC calls are replaced. The fake contains no pairing
validation, conflict detection, install decisions or fault-latch policy.
No PlatformIO, network, device or real Flash is accessed. Build outputs use a
temporary directory. Sanitizer mode enables ASan and UBSan with fatal errors.

## Fault Model

- Persistent namespaces, typed values, access modes and per-handle pending writes.
- Successful writes can become durable at set or commit; both modes are tested.
- A set or commit error can leave storage untouched or persist the full value.
- Faults target an operation and its one-based occurrence. Every injected fault
  must be reached; an unused fault fails the test.
- Query/read lengths, read errors, open errors and MAC errors are injectable.
  Read failures may dirty the SDK output buffer without changing persisted data.
- Hooks introduce records at RW open or change data/hardware before readback.
  These are test-side environmental changes, not production erase/write calls.
- Simulated reboot discards handles, pending writes and injections but preserves
  durable namespaces and the MAC. Tests construct a new production installer.
- Invalid handles, writes through RO handles and all erase APIs abort. Separate
  existing readonly fakes are neither reused nor weakened.

## Assertions And Limits

Tests assert result enums, unchanged caller output on load failure, exact bytes,
no leaked handles, read-only idempotence, conflict preservation, sticky errors
without any further SDK calls, reboot retries and preservation of unrelated
namespaces/keys. Successful installation also checks the full SDK call order.

The fake is not an ESP-IDF Flash emulator and does not prove torn-write recovery,
NVS capacity, real power-loss durability or concurrent-writer correctness.
Production requires one controlled owner per MCU. The owner must retain the
same installer for the commissioning run; a new object is only used here to
model reboot. Legacy-context import and MQTT isolation gates remain outside
this primitive and outside this suite. Real two-SDK power-loss/NVS-full tests
still require separately authorized hardware work.
