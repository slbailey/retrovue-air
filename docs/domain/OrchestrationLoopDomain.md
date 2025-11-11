_Metadata: Status=Draft; Scope=Domain; Owner=@runtime-platform_

_Related: [Architecture Overview](../architecture/ArchitectureOverview.md); [Playout Control Domain](PlayoutControlDomain.md); [Metrics and Timing Domain](MetricsAndTimingDomain.md)_

# Domain - Orchestration Loop

## Purpose

Define the coordinated real-time loop that advances FrameProducer → Renderer → Metrics processing under MasterClock discipline, ensuring deterministic playout.

## Role in runtime

- Acts as the scheduler that sequences decode, render, and telemetry tasks for each channel.
- Enforces bounded latency between producer output and renderer consumption.
- Mediates back-pressure signals to avoid buffer underrun/overrun conditions.

## Interfaces

- `AttachChannel(channel_context)` – Registers producer, renderer, and metric endpoints with loop state.
- `Start()` / `Stop()` – Begins or halts orchestration threads; guarantees graceful teardown within 500 ms.
- `Tick()` – Core loop iteration invoked at MasterClock-aligned cadence; fetches frames, schedules renders, updates metrics.
- `OnBackPressure(event)` – Handles buffer fullness/emptiness; adjusts pacing or triggers Playout Control state transitions.
- `SubscribeClock(master_clock)` – Registers for drift updates and scheduling notifications.

## Integration points

- **MasterClock** – Supplies cadence ticks and drift notifications to adjust pacing.
- **FrameProducer** – Provides frames and exposes buffer depth telemetry; loop requests decode work ahead of deadlines.
- **Renderer** – Consumes frames in order; loop coordinates render start times and monitors success.
- **Metrics Export** – Receives per-iteration telemetry (latency, jitter, queue depth) without blocking loop progress.
- **Playout Control** – Issues lifecycle commands; loop responds by altering scheduling or flushing buffers.

## Guarantees

- Producer-to-renderer latency ≤ 33 ms (p95) for 30 fps channels, configurable per profile.
- Frame deadlines never missed due to cumulative drift; loop applies corrective pacing using MasterClock drift data.
- Back-pressure resolved within three ticks; otherwise escalated to Playout Control for state transition.
- Loop starvation detection triggers fail-safe in ≤100 ms.

## Failure modes

- **Loop starvation** – Scheduling thread blocked; watchdog promotes channel to `Error` and emits alarm.
- **Misaligned tick phase** – Clock subscription lag; loop resynchronizes using last known drift, logs metric.
- **Buffer underrun** – Producer falls behind; loop transitions renderer to buffering and requests Playout Control recovery.
- **Overrun** – Renderer lags; loop throttles producer and raises metrics to prompt investigation.

## See also

- [Playout Control Domain](PlayoutControlDomain.md)
- [MasterClock Domain](MasterClockDomain.md)
- [Metrics Export Domain](MetricsExportDomain.md)

