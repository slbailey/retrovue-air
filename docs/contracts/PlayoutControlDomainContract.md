_Metadata: Status=Draft; Scope=Contract; Owner=@runtime-platform_

_Related: [Playout Control Domain](../domain/PlayoutControlDomain.md); [Architecture Overview](../architecture/ArchitectureOverview.md); [Metrics Export Domain](../domain/MetricsExportDomain.md)_

# Contract - Playout Control Domain

## Purpose

Define enforceable guarantees for session control covering state transitions, latency tolerances, and fault telemetry.

## CTL_001: Deterministic State Transitions

**Intent**  
Ensure the control plane only performs legal transitions from the published matrix and records violations.

**Setup**  
Channel initialized with valid schedule. Controller instrumented to observe `playout_control_state_transition_total` and `playout_control_illegal_transition_total`.

**Stimulus**  
Issue every legal command sequence once (`BeginSession`, `Pause`, `Resume`, `Seek`, `Stop`, `Recover`), then attempt an illegal transition (e.g., `Paused → Idle`).

**Assertions**

- Legal transitions move the channel through expected states with no intermediate states skipped.
- `playout_control_state_transition_total{from="<X>",to="<Y>"}` increments exactly once per successful transition.
- Illegal request rejected with no state change and increments `playout_control_illegal_transition_total{from="Paused",to="Idle"}` within 1 s.

**Failure Semantics**  
If any illegal transition proceeds, controller enters `Error`, emits `playout_control_illegal_transition_total`, and surfaces a critical alert.

## CTL_002: Control Action Latency Compliance

**Intent**  
Guarantee pause/resume/seek/stop latencies remain within documented tolerances while preserving ordering.

**Setup**  
Running channel in `Playing` state with MasterClock drift within nominal bounds. Metrics exporter captures latency histograms.

**Stimulus**  
Execute sequentially: `Pause`, `Resume`, `Seek` (forward 5 s), `Stop`, recording wall-clock deltas between command UTC and state change.

**Assertions**

- `playout_control_pause_latency_ms` p95 ≤ 33 ms; renderer halts on next frame boundary.
- `playout_control_resume_latency_ms` p95 ≤ 50 ms; first frame post-resume aligns with scheduled deadline.
- Seek completes (`Playing` → `Buffering` → `Playing`) in ≤ 250 ms end-to-end; resume latency logged.
- `playout_control_stop_duration_ms` ≤ 500 ms; final state `Idle`.

**Failure Semantics**  
Breaching any threshold triggers `playout_control_latency_violation_total` and escalates channel to `Error` with requirement for manual `Recover`.

## CTL_003: Command Idempotency & Failure Telemetry

**Intent**  
Validate `(channel_id, command_id)` deduplication window and failure telemetry coverage.

**Setup**  
Channel in `Playing`. Dedup window configured for 60 s. Metrics exporter watching `playout_control_timeout_ms`, `playout_control_queue_overflow_total`, `playout_control_recover_total`.

**Stimulus**  
Send duplicate `Seek` command with same `command_id` within 5 s, then simulate external control timeout (> SLA) and command queue overflow (flood of commands).

**Assertions**

- Duplicate command acknowledged without side effects; state remains `Playing`; no additional transition metric increment.
- Timeout forces controller to `Error`, increments `playout_control_timeout_ms` histogram bucket and requires subsequent `Recover` to exit error.
- Overflow increments `playout_control_queue_overflow_total` while channel stays in current state and throttles new commands for ≥100 ms.

**Failure Semantics**  
If duplicates mutate state or telemetry missing, controller marks channel `Error` and raises `playout_control_consistency_failure` alert.
