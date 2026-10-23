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
Each holds expectation-active state, expected-since monotonic time, last actual
progress time and publication sequence. Activation resets progress evidence;
retirement preserves the last pulse as inactive historical evidence. A revision
brackets updates; bounded reader retries accept a stable even revision or report
unavailable evidence. Readers never combine a previous scope's pulse with a new
scope's expectation. The owner identity is producer-local and is not a new wire
protocol or per-operation event.

Producer and observer must be rebuilt together for this current layout; structure
size validation rejects the preceding layout. No historical reader is provided.

The current layout assigns breadcrumb IDs 10 (`configuration_loaded`), 12
(`output_ready`), 22 (`stream_started`), and 23 (`stream_stopped`). Numeric gaps
are intentional; removing an obsolete producer probe does not change the
fixed structure layout. Unknown IDs remain safe to read.

The breadcrumb ring contains 128 slots. Every logical record is exactly 32
bytes:

```cpp
timestamp_ns, event_id, flags, source_id, arg0, arg1
```

Writers reserve a monotonically increasing ticket and make one atomic attempt
to claim its slot. A busy slot or an equal/newer committed ticket rejects the
publication; writers never wait or retry. The owner marks the slot in-progress,
writes its atomic numeric payload, then commits it. A release fence after the
claim and an acquire fence before the reader's final commit check prevent a
replacement payload from being accepted with the preceding commit. Readers
accept only a matching ticket and stable completed commit.

This is a bounded, best-effort diagnostic ring: completed older records may be
overwritten, contended publications are dropped, and ticket gaps are valid. A
stalled owner can prevent reuse of its slot; other slots continue independently.
Commit encoding supports positive tickets through `2^63 - 1`; publication rejects
zero or exhausted tickets rather than aliasing old commits. No delivery or
per-record latency guarantee is implied.

Human-readable channel/event names exist only in Monitor-side lookup functions.
Unknown event IDs remain safe and display as `unknown`.

## Lease behavior

The watcher renews a three-second lease. A producer checks the cheap watcher
flag first, validates the lease only when present, and clears an expired flag.
There is no reconnect queue: stale diagnostic events are not replayed, and
producer memory cannot grow because Monitor is absent.

The observer retains its attachment/renewal epoch locally. After attachment or
lease expiry, only progress at or after that epoch is current evidence. The
watcher's existing stale interval bounds initial missing evidence. An ordinary
renewal does not reset it, and terminal producer evidence does not renew a lease.

The current client coalesces heartbeat writes to one update per channel per
500 ms. This is producer implementation policy, not part of the binary layout;
stale classification remains watcher-side policy.

Only the current binary layout is accepted. Configuration, preferences and
application recovery formats are separate from this diagnostic layout.

The current magic is `0x4d4f4e49544f5221` (`MONITOR!`). Producer and watcher
must agree on the discriminator, structure sizes and exact process identity.


## Durable recording format

This watcher-side format is separate from the shared-memory producer layout.
The only accepted format is UTF-8 NDJSON `peepbooth-observations`, version `1`.
The version identifies this current contract; no historical decoder or converter
exists. Every frame is a single newline-terminated JSON object with exact fields,
finite numbers, integer nanosecond timestamps and explicit `null` for unknowns.
CPU is the measured process percentage (possibly above 100 on multiple cores),
not a heartbeat or estimated operation completion. Environment strings preserve
OS bytes as JSON Latin-1 code points; unset fields are `null`.

| Frame | Fields |
| --- | --- |
| `session` | `type`, `format`, `version`, `target`, `watcher`, `started_ns`, `started_utc_ns`, `interval_ms`, `stale_after_ns`, `history_capacity`, `limits`, `environment` |
| `checkpoint` | `type`, consecutive `sequence` starting at 1, `observation`, `history_retained`, `history_overwritten`, `incidents_overwritten`, `active`, `closed` |
| `end` | `type`, `outcome`, `limit`, `checkpoints`, `prefix_bytes`, `crc32`, `ended_ns`, `first_unrecorded_ns`, `discarded_lower_bound`, `observation_exit_status` |

Identities contain `pid` and Linux `start_ticks`. Together with nullable boot ID,
watcher identity and session start times they locate the observed process and
interval; unavailable boot identity must not imply cross-machine uniqueness.
`started_utc_ns` anchors the monotonic clock for human interpretation, not a
replacement for observation timestamps. Environment contains nullable `boot_id`,
`system`, kernel `release` and `machine`. Limits contain `bytes`, `checkpoints`
and `frame_bytes`; frame limits include the newline.

An observation contains `identity`, `observed_at_ns`, nullable `status`, `memory`,
`cpu_percent`, `health`, `sampled_at_ns`, `trend` and `progress`. A null status is
an explicit detach gap with no measurements or progress evidence. Other statuses
are `not_attached`, `sampled`, `sample_unavailable`, `identity_changed` and
`exited`. Only sampled records contain measurements; unknown CPU is distinct
from measured zero. Memory contains `rss_bytes`, `high_water_bytes`, `swap_bytes`.
Trend contains `duration_ns`, signed `rss_delta_bytes`, `bytes_per_second` and
`sustained_growth`. Progress contains numeric lifecycle (1 starting, 2 running,
3 stopping, 4 clean exit), `observer_since_ns`, `stale_after_ns` and exactly four
channels in main/gui/core/worker order. Each channel retains `id`, `active`,
`coherent`, `timestamp_ns`, `sequence`, `expected_since_ns`.

Each incident retains `kind`, nullable `channel`, `state`, `ending`,
`matching_observations`, `first`, `last` and nullable `closing` observations,
using the [existing incident semantics](architecture.md#evidence-based-incidents).
A checkpoint holds at most six active and N closed episodes; overwrite counters
are cumulative saturating counters. Earlier incident states remain in earlier
checkpoints. These are bounded snapshots, not a new event stream or a guarantee
that every intermediate closed episode survives rollover.

Terminal outcomes are `process_exited`, `once`, `cancelled`, `identity_changed`,
`observation_error` or `limit_reached`. Ordinary endings have null `limit` and
`first_unrecorded_ns`, zero discarded lower bound and observation exit status
0 or 1. Limit endings name `bytes`, `checkpoints` or `frame_size`, identify the
first omitted observation, retain a lower bound of one omitted checkpoint and
have **null observation exit status**: live observation has not ended yet.
`ended_ns` is the capture boundary, not an inferred process-death time.
The observation exit status belongs to the watcher; attached-process exit codes
and signals are unavailable and are not inferred.

The terminal CRC is CRC-32/ISO-HDLC over the exact header and checkpoint bytes,
including newlines, excluding the terminal frame. `prefix_bytes` counts those
same bytes. The reader rejects unknown versions/fields, non-finite values,
foreign identities, malformed complete frames, backwards times/counters,
invalid bounds, missing sequence numbers, checksum mismatch and trailing data.
A missing terminal or torn final frame is a partial capture with an unknown tail,
not a successfully completed session. Last-known open episodes in partial or
limited captures have unknown subsequent outcomes. The reader validates evidence
structure and integrity; it does not recompute incident policy or run a replay.
