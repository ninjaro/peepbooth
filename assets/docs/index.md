# Monitor contracts

Monitor is an external local-process watchdog, not an analytics or tracing
framework. An external observer survives producer crashes and fatal stalls.

- [Architecture and observation ownership](architecture.md)
- [Current fixed shared-state protocol](protocol.md)

The C++ namespace and installed package are `monitor`. Applications use
`monitor::watchdog_client__client`; Qt integration uses
`monitor::watchdog_qt__qt`. Producer calls are inert outside the supported Debug
Linux, non-Android profile. Diagnostic evidence does not establish per-job health.
