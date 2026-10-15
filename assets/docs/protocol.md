# Fixed shared-state contract

The current contract is a local, same-user, fixed-size shared-memory layout. It
is not JSON, a network protocol, or an application metrics schema.

## Identity and layout

A producer creates `/monitor-watchdog-<uid>-<pid>` with mode `0600`. The
header contains:

- 64-bit magic;
- header and total structure sizes;
- PID, UID, and Linux process-start ticks;
- a bounded 32-byte process name;
- process lifecycle;
- watcher flag and monotonic lease deadline.

The watcher verifies all identity and size fields before enabling publication.
Process-start ticks prevent accidental attachment after PID reuse. A binary
layout has no version field or historical reader. Producers and watchers must
be rebuilt together; unrelated or obsolete mappings are rejected rather than decoded.

## Frequently updated state

Four heartbeat slots correspond to `main`, `gui`, `core`, and `worker`.
Each holds active state, monotonic timestamp, and sequence.

The current layout assigns breadcrumb IDs 10 (`configuration_loaded`), 12
(`output_ready`), 22 (`stream_started`), and 23 (`stream_stopped`). Numeric gaps
are intentional; removing an obsolete producer probe does not change the
fixed structure layout. Unknown IDs remain safe to read.

The breadcrumb ring contains 128 slots. Every logical record is exactly 32
bytes:

```cpp
timestamp_ns, event_id, flags, source_id, arg0, arg1
```

Writers reserve a monotonically increasing ticket, mark the target slot
in-progress, write its atomic numeric payload, then commit it. Readers accept a
slot only when ticket and commit values are stable before and after the read.
Overwrite is intentional bounded behavior.

Human-readable channel/event names exist only in Monitor-side lookup functions.
Unknown event IDs remain safe and display as `unknown`.

## Lease behavior

The watcher renews a three-second lease. A producer checks the cheap watcher
flag first, validates the lease only when present, and clears an expired flag.
There is no reconnect queue: stale diagnostic events are not replayed, and
producer memory cannot grow because Monitor is absent.

The current client coalesces heartbeat writes to one update per channel per
500 ms. This is producer implementation policy, not part of the binary layout;
stale classification remains watcher-side policy.

Only the current binary layout is accepted. Configuration, preferences and
application recovery formats are separate from this diagnostic layout.

The current magic is `0x4d4f4e49544f5221` (`MONITOR!`). Producer and watcher
must agree on the discriminator, structure sizes and exact process identity.
