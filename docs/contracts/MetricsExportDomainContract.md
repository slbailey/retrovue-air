_Metadata: Status=Draft; Scope=Contract; Owner=@runtime-platform_

_Related: [Metrics Export Domain](../domain/MetricsExportDomain.md); [Metrics and Timing Domain](../domain/MetricsAndTimingDomain.md); [Telemetry Guidelines](../_standards/documentation-standards.md)_

# Contract - Metrics Export Domain

## Purpose

Enforce the telemetry pipeline guarantees for non-blocking submission, schema integrity, and delivery across transports.

## MET_001: Non-Blocking Export Semantics

**Intent**  
Ensure metric producers never block and hot-path latency stays within 5 μs on average.

**Setup**  
Instrumentation wraps `Record(sample)` to measure call latency; monitor `metrics_export_submission_queue_overflows_total`.

**Stimulus**  
Generate 10⁶ metric submissions per channel per second while exporters run under normal load.

**Assertions**
- Average `Record` latency ≤ 5 μs; p99 ≤ 15 μs.
- No caller thread performs I/O; ring buffer occupancy remains below high-water mark (≤ 80%).
- Queue overflow counter stays at 0; if watermark reached, only lowest-priority metrics are dropped with warning.

**Failure Semantics**  
Violation increments `metrics_export_submission_block_total` and triggers rate limiting on offending producers until remedied.

## MET_002: Schema Version Integrity

**Intent**  
Guarantee descriptor versions remain consistent and deprecated metrics follow retirement policy.

**Setup**  
Register metrics with varying descriptor versions; enable `metrics_export_descriptor_evictions_total` monitoring.

**Stimulus**  
Perform descriptor updates: (1) minor version bump with additive label, (2) major version bump breaking schema, (3) retire metric after deprecation window.

**Assertions**
- Consumers receive updated descriptor with correct semantic version before new samples emit.
- At most one active descriptor per metric name; stale descriptors marked with `deprecated=true` for configured retirement window.
- No descriptor eviction occurs before 30-day retention; if eviction forced, metric increments eviction counter and logs warning.

**Failure Semantics**  
Version mismatch raises `metrics_export_descriptor_version_skew_total` and halts sample emission for the affected metric until reconciled.

## MET_003: Delivery Reliability per Transport

**Intent**  
Validate transport-specific guarantees (streaming at-least-once, scrape best-effort, file exactly-once) with latency bounds.

**Setup**  
Enable gRPC streaming, Prometheus scrape, and file sink targets. Observe metrics: `metrics_export_delivery_failures_total{transport}`, `metrics_export_latency_ms{transport}`.

**Stimulus**  
Simulate network hiccups (drop gRPC packets), slow scrape interval (2 s jitter), and file I/O delay (200 ms) over a 5-minute window.

**Assertions**
- gRPC stream retries deliver duplicate-flagged samples with monotonic sequence IDs; at-least-once guarantee preserved; latency p95 ≤ 250 ms.
- Prometheus scrape produces consistent snapshots within 1 s of scrape time; missed scrape increments best-effort warning but no backlog induced.
- File sink flush produces exactly-once delivery per invocation; end-to-end latency ≤ 500 ms.
- Delivery failure metrics increment once per disrupted event and recover automatically.

**Failure Semantics**  
Persistent delivery failure raises `metrics_export_delivery_alert` and isolates affected transport while keeping submission path non-blocking.

