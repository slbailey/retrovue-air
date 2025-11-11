_Metadata: Status=Draft; Scope=Domain; Owner=@runtime-platform_

_Related: [Architecture Overview](../architecture/ArchitectureOverview.md); [Metrics and Timing Domain](MetricsAndTimingDomain.md); [Telemetry Guidelines](../_standards/documentation-standards.md)_

# Domain - Metrics Export

## Purpose

Define the abstraction that captures runtime metrics and exposes them to downstream systems (Prometheus scrape, gRPC streaming, file sinks) without coupling producers to transport concerns.

## Role in runtime

- Aggregates counters, gauges, and histograms emitted by MasterClock, FrameProducer, Renderer, and control loops.
- Normalizes labels, units, and naming conventions before forwarding to concrete transports.
- Manages push and scrape delivery guarantees with bounded buffering and back-pressure policies.

## Interfaces

- `RegisterMetric(descriptor)` – Declares a metric with type, unit, label schema, and retention policy.
- `Record(sample)` – Non-blocking submission path used by runtime components; MUST succeed in ≤5 μs average hot-path latency.
- `Subscribe(stream_config)` – Provides a streaming interface (e.g., gRPC) for external consumers with configurable aggregation cadence.
- `Flush(endpoint)` – Performs best-effort push for sinks that require explicit flush (e.g., HTTP POST).
- `GetSnapshot(filter)` – Returns a consistent snapshot for scrape-based collectors (Prometheus pull model).

## Integration points

- **MasterClock** – Publishes drift, jitter, and state coverage metrics.
- **FrameProducer** – Reports buffer depth, decode latency, and error counts.
- **Renderer** – Emits render cadence, drop counts, and correction telemetry.
- **Playout Control** – Logs state transitions, API latencies, and failure counters.

## Guarantees

- Metric submissions never block producer threads; back-pressure uses bounded lock-free queues with overwrite policy for non-critical metrics.
- Label sets remain stable across transports; missing labels default to explicit `unknown` tokens.
- Export latency capped at 250 ms (p95) for streaming sinks and 1 s for scrape snapshots.
- Retries use idempotent semantics; duplicate samples carry monotonic sequence IDs.

### Descriptor lifecycle & retention

- Metric descriptors carry a semantic version (`major.minor.patch`). Increment `major` for breaking schema changes, `minor` for additive labels, `patch` for descriptive fixes.
- Runtime stores up to 30 days of descriptor metadata and last-seen samples in memory; eviction follows LRU with alerts via `metrics_export_descriptor_evictions_total`.
- Deprecating a metric requires a retirement window where samples continue streaming with `deprecated=true` label until consumers acknowledge removal.

### Concurrency model & delivery guarantees

- Submission path (`Record`) executes on caller threads, enqueuing into a wait-free ring buffer with per-transport fan-out workers.
- Dedicated exporter threads pull batches every 10 ms (configurable) and invoke transport-specific serializers; no producer thread performs I/O.
- gRPC streaming transport guarantees **at-least-once** delivery with sequence replay; HTTP scrape model is **best-effort** with consistent snapshots; file sink is **exactly-once** per flush invocation.
- Clock alignment: streaming cadence targets 10 Hz and aligns first batch to the next MasterClock deadline to maintain deterministic sampling offsets.

## Failure modes

- **Exporter stall** – Downstream transport blocks; buffering reaches watermark, triggering shed of lowest-priority metrics and raising `metrics_exporter_stall` alert.
- **Metric registry overflow** – Exceeding registered metric limit; new registrations rejected, flagged via health check.
- **Schema drift** – Producer emits mismatched labels; schema validator rejects and logs structured error.
- **Transport error** – gRPC/HTTP failures; system retries with exponential backoff and exposes failure counters.

## See also

- [MasterClock Domain](MasterClockDomain.md)
- [Metrics and Timing Domain](MetricsAndTimingDomain.md)
- [Runtime Telemetry Standards](../_standards/documentation-standards.md)
