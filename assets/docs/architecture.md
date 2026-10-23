# Watchdog architecture

## Boundary

Ordinary producer code sees only the narrow client API:
process start, scoped progress ownership, passive heartbeat, manual channel-active
state, and numeric breadcrumb calls.
Transport names, shared-memory layout, watcher leases, process sampling, trend
analysis, and console presentation remain behind that boundary.

The external process is essential. An in-process observer disappears with the
crash or fatal stall it is meant to diagnose.

## Components

The fixed contract, Linux process/shared-memory implementation, producer client,
watchdog runtime, console orchestration and optional Qt adapter are separate
artifacts. The Qt-free client and runtime do not acquire a Qt dependency merely
because the optional GUI heartbeat adapter uses Qt.

One constant in the client boundary enables publication only in Debug Linux
builds outside Android. The same application calls compile to no-ops elsewhere
without constructing the process client. In an enabled build, a null state or
absent watcher flag returns before the monotonic clock is read. A watcher lease
expires automatically, so a killed Monitor cannot leave producer publication
enabled.

The host owns producer lifetime: start before workers or timer callbacks, stop
and join those producers, then stop or destroy the client. Concurrent heartbeat
and breadcrumb calls are supported while running; concurrent unmapping is not.
`stop()` is idempotent. A later successful start creates fresh heartbeat throttle
state. Observers may retain the final clean-exit mapping, but only the producer
unlinks its endpoint. A Qt host destroys its windows and joins publishing workers,
then destroys the GUI heartbeat adapter, then permits process-client teardown.
The adapter owns its timer and cancels receiver-bound queued callbacks when
destroyed. It does not retire other heartbeat channels. Failed producer startup
leaves its timer inactive; a later successful start may create a fresh adapter.
A detached or expired observer leaves no event backlog;
reattachment resumes current publication.

## Expected progress ownership

One exclusive owner may claim each fixed channel with
`client::expect_progress(channel)` or the Debug-only `expect_progress(channel)`
boundary. An invalid handle explicitly rejects an occupied channel, invalid
channel or unavailable producer. A channel represents one independently supervised
domain; it is not a collection of unrelated operations.

The move-only handle starts an expectation without claiming progress. Only its
`heartbeat()` can publish for that scope. Ordinary channel pulses and manual
activation/deactivation cannot satisfy or retire it. Move transfers ownership;
`reset()`, destruction, cancellation through reset and producer shutdown retire it.
Restart cannot revive old handles. Reset may overlap heartbeat calls and rejects
late publication before returning. A handle may safely outlive its client but
then becomes inert. Producers must still finish publishing before client unmapping
or moving/destroying a handle they reference.

Without an explicit expectation, a pulse is passive evidence, never a promise of
future work. `set_channel_active` remains a manual aggregate lifecycle boundary
for existing hosts; it cannot isolate unrelated operations. Activation neither
publishes progress nor refreshes an unchanged expectation. New independently
supervised work uses the scoped handle. Idle/no-input periods retire the scope;
heartbeats must come from actual progress, not a timer unrelated to the work.

The Qt adapter owns the GUI channel exclusively. A rejected second adapter starts
no timer and cannot retire the first. Timer delivery is GUI progress only.
The four fixed slots and bounded breadcrumb ring are unchanged in number. There
is no operation dictionary, event queue or allocation per pulse. Suppressed or
disconnected pulses take no lock; potential publications use a single nonblocking
lock attempt and may be dropped on contention. Lifecycle transitions serialize
with publication. The existing 500 ms coalescing interval remains unchanged.

## Failure interpretation

Active heartbeats are compared with a configurable stale interval.

- GUI stale, non-GUI current: likely GUI/event-loop stall.
- Another mixture of fresh and stale active channels: degraded heartbeat
  evidence; the fresh channel does not establish complete health.
- All active channels stale and CPU low: likely blocking wait, deadlock, or
  global idle stall.
- All active channels stale and CPU high: likely spin, busy loop, or runaway
  work.
- Running producer with no expectation: `IDLE`, not a hang.
- Pending first progress: `AWAITING_HEARTBEAT` until the configured stale interval
  expires; this is not fabricated healthy evidence.
- An incoherent concurrent snapshot: `EVIDENCE_UNAVAILABLE`, not healthy or hung.

These categories describe the explicitly expected domains, not every application
operation. Inactive channels are ignored. Missing first progress is timed from
the later of expectation creation and observer attachment. Reattachment or an
expired observer lease starts a fresh evidence interval, without treating old
pulses as current progress; ordinary polling never extends that grace. The same
configured stale interval governs subsequent progress. Its inclusive boundary
remains fresh. No new timeout is chosen here; callers must allow for publication
coalescing and their promised progress cadence.

Detachment stops pulse/breadcrumb publication but does not cancel real host work.
Lifecycle metadata remains current. Stopping/clean
producer lifecycles have no live expectation; retained final evidence is idle.
Actual process exit remains a separate observation with no current responsiveness
claim. Legacy hosts aggregating unrelated work into one manual channel still
cannot infer per-operation health from it.

The move-only `observed_process` owner retains one process handle, session/lease,
CPU baseline, bounded memory trend and latest raw/derived observation. It is used
serially; moving transfers the lease and destruction releases it. Sampling and
health use the observation's fixed monotonic instant. The first CPU delta is
unavailable rather than measured zero. PID/start-ticks mismatch or missing samples
clear the baseline; recovery starts a new interval. This is not an atomic
cross-process snapshot. `poll()`/`latest()` views must be copied if they need to
outlive the next poll. `try_attach()` renews the observer lease and is not a
read-only discovery probe. Console retry/cadence and formatting remain frontend
policy, with no process or clock reads during formatting.

## Bounded observation history

`observation_history` is a separate serial watcher-side value store. The caller
supplies an exact PID/start-ticks identity and an explicit record capacity; zero
disables retention. It retains compact observation timestamps, statuses, available
RSS/HWM/swap, CPU, sampled trend, channel evidence, watcher policy and responsiveness.
Unknown CPU remains unavailable. Non-sampled
statuses never inherit sampled metrics or health. It rejects other incarnations
and backwards timestamps, overwrites the oldest record at capacity and saturates
its overwrite counter. There is no producer buffer, polling thread, timeout
override, severity or escalation policy in this store.

`note_gap()` records an explicit frontend disconnection with no sample/status or
health. Time between observations is unobserved; gaps are never interpolated,
and no cadence threshold is inferred. The store is not persistent recording,
automatic reconnect, breadcrumb history or proof of continuity. A caller may
retain a separate history when starting a new observation interval.

`monitor --pid PID --history N` opts into the last N observations/gaps, printed
on exit, failure or cancellation with the overwrite count. The console records
detach as a gap; unavailable/identity-change/exit observations retain their real
status. All/name cohort mode does not support this option. With no live progress
expectation, the console explicitly states that process observation provides no
application-progress coverage; `IDLE` must not be read as proof of job health.
Single-PID watching continues after unavailable resource samples, preserving
their diagnostics and returning 1 for the incomplete session even after recovery
or exit. Identity changes stop observation; once mode makes only one attempt.

## Evidence-based incidents

`observation_history` feeds its accepted records into a serial `incident_tracker`.
It retains at most N closed incidents, with the same explicit N as observation
capacity, and at most one active episode per fixed channel plus one RSS and one
uncertainty episode. Zero disables both. Counters saturate rather than wrap;
closed-incident rollover has its own overwrite count. First, last qualifying and
ending evidence are value copies, surviving observation rollover. Each includes
the exact process incarnation and actual observation time. These times describe
when a finding was seen, not its unobserved causal start or duration guarantee.
API views are borrowed until the next update; copy values needed beyond updates
or closed-incident rollover.

| Kind | Qualifying evidence | Meaning |
| --- | --- | --- |
| `overdue_progress` | A coherent active channel, known expectation/observer start and actual configured stale boundary exceeded | Missing expected channel progress, not proof of a deadlock or an individual operation's health |
| `sampled_rss_growth` | The existing memory-trend rule reports growth over a window wholly inside the observed interval | A sampled resource trend, never a memory-leak diagnosis |
| `unavailable_evidence` | Missing/unavailable sampling or incoherent watchdog evidence | Diagnostic uncertainty, never fabricated healthy or hung state |
| `process_exit` | The observer reports that the selected process incarnation exited | A lifetime fact, never an operation failure or final RSS sample |

Repeated qualifying observations update one episode. Fresh evidence clears a
finding; this is not an application-success receipt. A later heartbeat observed
between two overdue samples ends the previous episode with `progress_observed`
and begins a new one, without claiming continuous failure across the checkpoint.
Retirement, replacement and producer shutdown interrupt a channel episode.
Missing evidence, observer detachment, reattachment, identity loss and process
exit end unresolved findings as unknown. Recovering measurements resolves only
the uncertainty episode, never retrospectively resolves an interrupted finding.
Explicit detachment without an open finding creates no new incident.

An observer-interval change breaks continuity. RSS windows that still include
pre-gap samples remain available as raw trends but cannot start a new incident
until the complete window belongs to the new observed interval. CPU inactivity,
high RSS, unknown initial CPU and ordinary idle alone start no finding. Normal
exit is descriptive. Incidents have no new severity levels, thresholds, alerts
or effect on the console's existing exit-status policy.

The console prints incident summaries and their bounded first/last/ending
evidence with `--history N`. There is no per-operation registry or producer
traffic. Channels have no exported owner identity: observed expectation timestamps
detect replacements when visible, but cannot prove every intervening transition.
The watcher cannot distinguish manual aggregate activation from scoped ownership;
only the host contract identifies the declared channel's domain.
There is no persistent/crash-safe recorder, automatic incident escalation or
recovery of events outside the sampling/retention window.

## Read-only process discovery

On desktop Linux, `monitor --list` inventories current-user endpoints under
`/dev/shm` without attaching an observer, renewing or clearing leases, sampling
resources, starting workers, or removing names. Inspection uses read-only access
and checks the file's owner, type and size, published lifecycle, current layout,
and live PID/start-ticks identity. Names are bounded; rows are sorted by PID and
include start ticks. Equal names remain separate identities. Control characters
in displayed names cannot create additional rows.

Malformed or inaccessible matching endpoints produce diagnostics and a nonzero
CLI result, rather than an apparently complete empty inventory. Other users'
endpoint names are ignored. An endpoint can disappear after discovery; attaching
an observer must still revalidate its exact identity. Listing grants no ownership
or permission to remove another process's endpoint. Unsupported platforms report
that discovery is unavailable. Inspection resides outside the producer archive
object, so linking a producer client does not pull observer discovery code.

## Observation registry

A serial watcher-side `observation_registry` owns one `observed_process` for each
explicit PID/start-ticks pair. It revalidates the live incarnation before acquiring
a lease. Repeated selection reuses the owner; a stale identity cannot replace it.
Detachment matches both identity fields and releases only that entry. Moving the
registry transfers ownership. No producer endpoint is removed by the registry.

Polling returns value snapshots in identity order, with separate CPU baselines,
trends and leases. Each process retains its own observation instant; the batch is
not a simultaneous cross-process snapshot. Exit evidence remains available until explicit removal. The
frontend owns membership, retry, cadence and resource budgets; the registry adds
no polling thread, implicit discovery, fixed process cap or automatic eviction.
To observe an inventory, the frontend explicitly attaches its selected identities.

Friendly-name selection requires a complete inventory and exactly one exact name
match. Missing, ambiguous or incompletely inspected names are errors. Selection
itself acquires no lease; subsequent attachment still revalidates identity. The
console accepts exactly one of an explicit PID, an unambiguous exact name, or all
members of a complete startup inventory. An empty cohort or incomplete discovery
is an error. All/name mode attaches the selected exact identities, releases earlier
attachments if a later member fails, and keeps that fixed cohort without adopting
later arrivals. A failed resource sample is an error, not a silently omitted member.
Each exit is reported before detachment; other members continue until the last
exits, the caller requests cancellation, or a failure occurs. Once mode takes one
batch and releases its leases. Foreign-process exit codes and signals are unavailable.

The frontend retains the existing configurable polling interval. It waits on one
member's lifetime handle between batches; another member's exit is observed at
the next poll. This adds neither a worker nor an adaptive scheduling policy.
The shared endpoint supports only one watcher per producer. Multiple watchers
of the same producer remain unsupported; a multi-process cohort does not change
that ownership restriction.

## Memory interpretation

Monitor samples RSS, high-water RSS, swap, process CPU ticks, and start identity
from `/proc`. Samples live in a bounded time window. Least-squares RSS slope,
net change, repeated increases across both halves of the observation period,
and the proportion of nondecreasing adjacent samples identify sustained growth.
The classifier observes at least five seconds (or the complete configured
window when shorter), so startup allocation steps followed by a plateau are not
reported as sustained growth.

The current sampler does not collect `smaps_rollup` or perform heap profiling.


## Opt-in durable recording

The single-PID console can explicitly record the same bounded watcher values.
Recording is disabled by default and has no producer-side file I/O. It adds no
progress channel, classification, timeout or severity rule. It stores no
breadcrumbs, application payloads, SVG bytes, images or unrestricted memory.

The caller must supply a history capacity, total file-byte limit, checkpoint
limit and encoded-frame-byte limit. There are no implicit recording limits.
Memory is bounded by the existing N observations, N closed incidents, six active
slots and one bounded encoding buffer (plus fixed metadata); this is not an RSS
quota. File-byte limits include header and terminal records, not filesystem
allocation or journal overhead. Each checkpoint stores one observation and the
current incident snapshot, not a copy of the entire raw history.

Each complete NDJSON frame is flushed and synced synchronously on the watcher.
This can delay observation; cadence is not a recording guarantee. Evidence is
never interpolated. At a count, byte or frame limit, recording ends with an
explicit limit marker, first omitted observation time and a discarded lower
bound of one. Later evidence and the eventual observation outcome are unknown;
watching continues, but the console ultimately returns 1. Limits never rotate,
overwrite old evidence or silently discard new frames as if capture continued.

A fresh, private `PATH.partial` is created exclusively; existing final/partial
files and leaf symlinks are rejected. Parent directories must already exist. The initial header and partial directory
entry are synced before capture is acknowledged.
Normal exit, once mode, cancellation and observation failure append a detach
gap, its incident endings and an explicit terminal outcome. After file sync and
close, no-clobber rename publishes `PATH`; its directory is then synced. A write,
sync, close or publication error stops capture, reports a diagnostic and retains
evidence where possible. Observation continues. A directory-sync failure attempts
to restore the partial name; if that fails, durability is explicitly unknown.
No destructor invents finalization. Abrupt termination leaves an incomplete
prefix; even a terminal frame under a `.partial` name is not published success.

The current [recording format](protocol.md#durable-recording-format) is read
without the original process. The reader enforces its own explicit input limits,
checks structure/counts/CRC and reports finalized, limited or partial capture.
Finalized capture does not mean healthy application or successful observation:
the terminal record separately retains the observation exit status. CRC is
corruption detection, not authentication or proof of filesystem persistence.
No reader can infer a missing tail, unobserved incidents or continuous health.
