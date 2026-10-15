# Watchdog architecture

## Boundary

Ordinary producer code sees only the narrow client API:
process start, heartbeat, channel-active state, and numeric breadcrumb calls.
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

## Failure interpretation

Active heartbeats are compared with a configurable stale interval.

- GUI stale, non-GUI current: likely GUI/event-loop stall.
- Another mixture of fresh and stale active channels: degraded heartbeat
  evidence; the fresh channel does not establish complete health.
- All active channels stale and CPU low: likely blocking wait, deadlock, or
  global idle stall.
- All active channels stale and CPU high: likely spin, busy loop, or runaway
  work.
- No active channel: awaiting first producer progress, not a false hang.

These categories describe publication freshness, not proof of expected work.
Inactive channels are ignored; an active channel without a timestamp is stale.
An age exactly at the configured boundary is fresh. A progressing contributor
can hide another contributor's stall when they share the same process channel;
arbitrarily deactivating it from one worker can hide other work.

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

## Memory interpretation

Monitor samples RSS, high-water RSS, swap, process CPU ticks, and start identity
from `/proc`. Samples live in a bounded time window. Least-squares RSS slope,
net change, repeated increases across both halves of the observation period,
and the proportion of nondecreasing adjacent samples identify sustained growth.
The classifier observes at least five seconds (or the complete configured
window when shorter), so startup allocation steps followed by a plateau are not
reported as sustained growth.

The current sampler does not collect `smaps_rollup` or perform heap profiling.
