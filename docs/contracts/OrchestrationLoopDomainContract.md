_Metadata: Status=Draft; Scope=Contract; Owner=@runtime-platform_

_Related: [Orchestration Loop Domain](../domain/OrchestrationLoopDomain.md); [Playout Control Domain](../domain/PlayoutControlDomain.md); [MasterClock Domain](../domain/MasterClockDomain.md)_

# Contract - Orchestration Loop Domain

## Purpose

Specify the real-time guarantees for coordinating producer, renderer, and metrics tasks under MasterClock pacing.

## ORCH_001: Tick Discipline & Latency Budget

**Intent**  
Ensure loop ticks honor MasterClock cadence and keep producer→renderer latency within 33 ms (p95).

**Setup**  
Channel attached to orchestration loop with MasterClock drift ≤ 0.1 ppm. Metrics `orchestration_tick_skew_ms` and `orchestration_latency_ms` recorded per tick.

**Stimulus**  
Drive ticks at 30 fps for 5 minutes, injecting ±0.2 ms jitter into MasterClock callbacks halfway through.

**Assertions**
- Tick skew (`orchestration_tick_skew_ms`) remains within ±1 ms for 99% of ticks.
- Producer→renderer latency histogram (`orchestration_latency_ms`) p95 ≤ 33 ms; no frame deadline missed.
- Any missed MasterClock callback triggers immediate catch-up tick without skipping sequence numbers.

**Failure Semantics**  
Exceeding thresholds increments `orchestration_tick_violation_total` and promotes channel to `Error`, prompting Playout Control intervention.

## ORCH_002: Back-Pressure Handling

**Intent**  
Validate that buffer underrun/overrun events resolve within three ticks and emit telemetry.

**Setup**  
Loop instrumented with `orchestration_backpressure_events_total{type}` and `orchestration_backpressure_recovery_ms`. FrameProducer/Renderer configured with shallow buffers to provoke events.

**Stimulus**  
Force an underrun (producer stalls) followed by an overrun (renderer throttled) while tick rate remains 30 fps.

**Assertions**
- On underrun: loop transitions renderer to buffering, notifies Playout Control, and restores steady state within ≤3 ticks; `orchestration_backpressure_events_total{type="underrun"}` increments.
- On overrun: loop throttles producer output, drains backlog within ≤3 ticks; corresponding metrics increment.
- Recovery time (`orchestration_backpressure_recovery_ms`) ≤ 100 ms for each event.

**Failure Semantics**  
If recovery exceeds bounds, loop raises `orchestration_backpressure_unresolved` alert and escalates to Playout Control for channel downgrade.

## ORCH_003: Starvation & Teardown Safety

**Intent**  
Guarantee starvation detection and graceful stop within 500 ms.

**Setup**  
Loop running with watchdog timer monitoring `orchestration_starvation_alert_total`. Channel in `Playing`. Metrics exporter capturing teardown duration.

**Stimulus**  
Block loop thread for 150 ms to simulate starvation, then issue `Stop()` command to initiate teardown.

**Assertions**
- Starvation detected within ≤100 ms; `orchestration_starvation_alert_total` increments and channel transitions to safe mode.
- Post-`Stop()`, loop flushes outstanding frames, releases resources, and reports `orchestration_teardown_duration_ms` ≤ 500 ms before signalling completion.
- Metrics exporters receive final latency snapshot before shutdown.

**Failure Semantics**  
If starvation goes undetected or teardown exceeds 500 ms, channel enters `Error`, raising `orchestration_teardown_violation` for operator response.

