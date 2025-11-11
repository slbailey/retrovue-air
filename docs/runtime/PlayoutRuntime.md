_Related: [Architecture overview](../architecture/ArchitectureOverview.md) • [RetroVue runtime - ChannelManager](../../Retrovue/docs/runtime/ChannelManager.md)_

# Playout runtime

## Purpose

Explain the execution model, threading, timing rules, and operational safeguards enforced by the playout engine at runtime.

## Channel lifecycle

- **Start:** ChannelManager calls `StartChannel`, providing the initial playout plan and TCP port allocation.
- **Update:** Delta schedules trigger `UpdatePlan`, which hot-swaps the decode graph without interrupting output.
- **Stop:** Planned maintenance or fatal errors invoke `StopChannel`, draining buffers before releasing resources.

## Threading model

- **Demux thread:** Reads packets via libavformat and pushes them into codec-specific queues.
- **Decode pool:** Configurable worker threads per codec profile (defaults: H.264 = 2, HEVC = 3) convert packets into frames.
- **Staging thread:** Packages frames with PTS/DTS, asset IDs, and duration before enqueuing to the Renderer ring buffer.
- **Telemetry loop:** Emits metrics and structured logs without blocking the decode path.

## Timing guarantees

- All timing decisions reference the RetroVue MasterClock delivered over the control API.
- Minimum frame lead time: 150 ms. Soft maximum: 500 ms. Exceeding the ceiling triggers buffer trimming.
- Slate insertion occurs when available frames drop below 30, preventing Renderer starvation.
- Renderer consumption is intentionally decoupled from decode timing, keeping output aligned with the MasterClock.

## Timing telemetry flow

- MasterClock publishes monotonic UTC deadlines and correction signals that gate Renderer pacing.
- Renderer compares in-flight frames against MasterClock deadlines before packaging transport stream output.
- Metrics exporter samples both MasterClock state and Renderer gap measurements to expose Prometheus gauges.

```mermaid
flowchart LR
    MC[MasterClock\n(now, drift_ppm, corrections)]
    R(Renderer\nframe pacing & buffering)
    M[Metrics Exporter\nPrometheus scrape]

    MC --> R
    R --> M
    MC --> M
```

## Resource management

- Every channel owns a memory budget for frame staging (default 90 frames is approximately 3 s at 30 fps).
- Libav contexts are pooled per codec to reduce reinitialization costs between plan updates.
- Backpressure from the Renderer ring buffer dynamically slows decode throughput to stay within the soft maximum.

## Health monitoring

- Metrics exported at `/metrics` (Prometheus format):
  - `retrovue_playout_channel_state{channel="N"}`: `ready`, `buffering`, or `error`.
  - `retrovue_playout_frame_gap_seconds{channel="N"}`: deviation from scheduled timestamps.
  - `retrovue_playout_restart_total{channel="N"}`: count of automatic decoder restarts.
- Structured logs include channel id, asset id, and timing drift for simplified correlation with RetroVue core logs.

## Failure handling

- Decoder crashes trigger automatic restart with exponential backoff (max five attempts per minute).
- Persistent failures transition the channel to `error` and notify ChannelManager via gRPC status.
- Slate playback remains active until ChannelManager delivers new playable content or disables the channel.

## Operator guidance

- Use the `--log-level trace` flag when tracing frame flow and timing adjustments.
- Collect a metrics snapshot before and after plan updates to verify buffer stability.
- When diagnosing timing issues, compare `retrovue_playout_frame_gap_seconds` against ChannelManager scheduler logs.

## See also

- [Architecture overview](../architecture/ArchitectureOverview.md)
- [Deployment integration](../infra/Integration.md)
- [RetroVue renderer runtime](../../Retrovue/docs/runtime/Renderer.md)
