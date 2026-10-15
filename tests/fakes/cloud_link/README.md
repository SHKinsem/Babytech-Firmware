# CloudLink Host I/O Tests

Object: the shared CloudLink transport and CloudSession implementation.
Milestone: v4 session-bound transport host validation.
Boundary: deterministic host control-flow tests, not device or broker acceptance.

From the babytech-firmware repository root:

```sh
python3 tools/test_cloud_link.py
python3 tools/test_cloud_link.py --sanitize
python3 tools/test_cloud_link.py --sanitize --case send-time-generation
```

The runner directly compiles `shared/BabytechCloudLink/src/CloudLink.cpp` and
`CloudSession.cpp`. Production headers, JSON classification, queue selection,
session validation and transport logic are not copied into the fakes. Only
Arduino/String, WiFi, Preferences, WebServer, PubSubClient and FreeRTOS I/O are
replaced. ArduinoJson uses the existing headers under
`device-controller/.pio/libdeps/motion/ArduinoJson/src`, or `--arduino-json PATH`.
Arduino String/Stream/Print/PROGMEM integrations are disabled; standard string
support stays enabled. No packages are downloaded and PIO/git are not invoked.
Compiler outputs live in an automatically removed temporary directory.

## Coverage

- Fail each of the mutex and five queue allocations, packet buffer allocation,
  and task creation three times, then retry the same CloudLink successfully.
  Assert no fake queue/mutex/task/Preferences handles remain before retry.
  Packet/task failures exercise a valid persisted settings fixture; retry with
  absent NVS must not resurrect the failed startup's configuration.
- Invalid identity, repeated begin/beginV4, and competing instances cannot
  replace the original worker, identity or callback owner.
- Real task entry performs authentication and both subscriptions, creates a
  v4 session, answers valid probes, rejects invalid probes/freshness, and expires
  at the exact 24-hour boundary. Legacy begin creates no v4 session/probe/token.
- Real registered MQTT callback handles bounded raw payloads, topic/device
  rejection, Stop priority and overflow eviction, retained context replacement,
  probe/receipt isolation, queue saturation and drop counters.
- Session-bound output checks size/topic limits, queue capacity, two sends per
  worker iteration, tags, success/failure and non-retained topic publication.
  The legacy publish API is rejected in v4 mode.
- WiFi loss, socket loss, loop failure, settings replacement, 24-hour expiry and
  explicit reconnect invalidate queued inbound generations and queued outbound
  payloads. Old payloads cannot be enqueued after reconnect; queued stale tagged
  payloads get false results. New tokens and generations differ from old ones.
- A specifically deferred queue insertion models a producer preempted after
  session validation and resumed after reconnect's queue drain. This exercises
  the separate generation check immediately before actual MQTT publication.
  Another case expires a probe-derived payload's session inside MQTT loop,
  after the pre-loop check but before publication.
- Authentication/subscription/random-token failures keep sessions unavailable
  and retry with backoff. A valid persisted settings fixture reaches MQTT.
  HTTP status is valid JSON with no username/password fields or credential
  values; AP access checks and failed NVS saves preserve their boundaries.
- Local `configure()` can save/read back before start with one provisioning
  owner and no other running CloudLink. It creates no worker, queues or mutex,
  performs no network I/O and is loaded by a later valid start. Running links
  still serialize saves and request worker reconnection. Both paths require
  bounded nonempty fields, nonzero ports and the existing DNS/IP host characters. ASCII controls and
  overlong/unterminated fields are rejected before storage. Maximum field sizes
  and both port boundaries are covered without exposing credentials in logs.
- Save and readback-open/length/read/content faults retain verified RAM and the
  working session, then permit retries. Readback faults can leave the candidate
  in NVS; the API does not claim rollback or erase it. Existing record layout,
  unrelated keys and worker-only socket ownership are checked.
- Real host threads pause a save before verification and exercise a waiting API
  writer, HTTP writer or reader, including a first-writer verification failure.
  The final RAM and record must agree. Configuration during an in-flight connect
  cannot make that old connection available; reconnect drops old queued traffic.

## Limits

- Every case is a separate process because production deliberately has
  MCU-lifetime singleton ownership and no shutdown API. xTaskCreate captures the
  production entry; a vTaskDelay callback drives bounded events and raises a
  test-only exception to stop the real run loop. No private members are exposed.
- Queue operations copy actual byte buffers with capacities and FIFO behavior.
  Semaphores use a host mutex/condition variable and detect invalid handles,
  recursive takes and unowned gives. Only the configuration transaction cases
  use overlapping OS threads; queues and general I/O fakes are not universally
  thread-safe. Deferred insertion remains a deterministic preemption model,
  not proof of all ESP32/FreeRTOS interleavings.
- Successful lifetime-long queues/tasks are cleaned up by the host fixture
  only after the worker has stopped. Startup failure resource assertions happen
  before that cleanup. A task failure retains one PubSubClient-owned 2560-byte
  packet buffer for reuse; it is bounded and destroyed with the fake client,
  not evidence that production immediately returns those bytes to the heap.
- ASan/UBSan check executed host code; they do not validate real PubSubClient
  allocation internals, ESP heap fragmentation, the 6144-byte task stack,
  scheduler timing, RNG quality, TCP/broker behavior, ACLs or Flash durability.
  Result-queue saturation/delivery guarantees and every malformed NVS record
  shape are not covered. There is no automatic production shutdown test.
- Device acceptance still requires task stack high-water measurements, repeated
  connect/disconnect and configuration changes under load, allocator failure
  observations, real MQTT callback delivery and NVS reset/power-loss checks.
  These host tests do not authorize deployment or flashing.
