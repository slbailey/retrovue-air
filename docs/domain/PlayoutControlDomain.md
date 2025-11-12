_Metadata: Status=Draft; Scope=Domain; Owner=@runtime-platform_

_Related: [Architecture Overview](../architecture/ArchitectureOverview.md); [MasterClock Domain](MasterClockDomain.md); [Playout Engine Contract](../contracts/PlayoutEngineContract.md)_

# Domain - Playout Control

## Purpose

Define the real-time session control responsibilities for channel playout, including start, pause, resume, stop, and seek orchestration under tight timing constraints.

## Role in runtime

- Owns the authoritative state machine for every channel/session.
- Issues lifecycle commands to `FrameProducer`, `Renderer`, and supporting services while preserving MasterClock alignment.
- Mediates external control inputs (automation, operator UI, API) and validates ordering/latency requirements before forwarding to runtime components.

## State machine

- States: `Idle`, `Buffering`, `Ready`, `Playing`, `Paused`, `Stopping`, `Error`.
- `Error` is _recoverable_ only via `Recover(channel_id, plan)` which returns the channel to `Buffering`; no other transitions originate from `Error`.
- All transitions are validated against MasterClock deadlines; e.g. `Playing → Paused` flushes in-flight frames at the next frame boundary, `Paused → Playing` schedules resume on an aligned PTS.
- Seek operations always pass through `Buffering` to guarantee fresh frames before re-entering `Playing`.

### Transition matrix

| From → To               | Allowed | Notes                                                                |
| ----------------------- | ------- | -------------------------------------------------------------------- |
| `Idle` → `Buffering`    | ✅      | `BeginSession`; primes producer buffers                              |
| `Buffering` → `Ready`   | ✅      | Buffer depth ≥ configured minimum                                    |
| `Ready` → `Playing`     | ✅      | Control plane issues start; align deadline ≤ 1 frame                 |
| `Playing` → `Paused`    | ✅      | Pause; frame boundary alignment required                             |
| `Paused` → `Playing`    | ✅      | Resume; restart deadline scheduled via MasterClock                   |
| `Playing` → `Buffering` | ✅      | Seek or underrun; renderer enters slate                              |
| `Buffering` → `Playing` | ✅      | Post-seek resume when buffers refilled                               |
| `Playing` → `Stopping`  | ✅      | Stop command; drain outstanding frames                               |
| `Stopping` → `Idle`     | ✅      | Final teardown complete                                              |
| `*` → `Error`           | ✅      | Fatal command/clock violation; emits escalation                      |
| `Error` → `Buffering`   | ✅      | Recover initiated; rehydrate buffers before resuming                 |
| Any other transition    | ❌      | Rejected; produces `playout_control_illegal_transition_total` metric |

### Latency tolerances (p95)

- `BeginSession` → first `Buffering`: ≤ 75 ms.
- `Paused` → `Playing`: ≤ 50 ms from resume UTC request to first emitted frame.
- `Playing` → `Paused`: ≤ 33 ms (one frame @30 fps) from pause UTC to render halt.
- `Seek` (`Playing` → `Buffering` → `Playing`): ≤ 250 ms overall; control plane reports measured latency on resume.
- `Stop` (`Playing` → `Stopping` → `Idle`): ≤ 500 ms to release resources.

## Integration points

- **MasterClock** – Provides UTC deadlines and drift data so control commands align with timing guarantees.
- **FrameProducer** – Receives start/stop/seek signals and produces frames accordingly.
- **Renderer** – Pauses or resumes output when instructed, respecting frame boundary semantics.
- **MetricsExporter** – Emits control-path latency, state transition counters, and error telemetry.

## Interfaces

- `BeginSession(channel_id, schedule)`
  - Preconditions: channel in `Idle`; schedule validated.
  - Effects: transitions to `Buffering`, primes producer, registers MasterClock epoch.
  - Postconditions: emits `playout_control_state_transition_total{from="Idle",to="Buffering"}`; command idempotent via channel `generation_id`.
- `Pause(channel_id, reason)`
  - Preconditions: state `Playing`.
  - Effects: schedules pause at next frame boundary, notifies renderer.
  - Postconditions: state `Paused`; logs `playout_control_pause_latency_ms`. Duplicate `command_id` rejected with `409 AlreadyProcessed`.
- `Resume(channel_id)`
  - Preconditions: state `Paused`.
  - Effects: schedules resume deadline using MasterClock drift correction.
  - Postconditions: state `Playing`; stamps `playout_control_resume_latency_ms`.
- `Seek(channel_id, target_pts, command_id)`
  - Preconditions: state `Playing` or `Paused`; `target_pts ≥ current_pts`.
  - Effects: transition to `Buffering`, flush buffers, prime producer at new PTS.
  - Postconditions: state returns to `Playing` once buffer depth restored; idempotent based on `command_id`.
- `Stop(channel_id, mode)`
  - Preconditions: not already `Idle`.
  - Effects: enters `Stopping`, drains producer/renderer, persists session summary.
  - Postconditions: state `Idle`; emits `playout_control_stop_duration_ms`. Repeated stop requests acknowledged without side effects.
- `Recover(channel_id, plan)`
  - Preconditions: current state `Error`.
  - Effects: resets command queue, revalidates schedule, returns to `Buffering`.
  - Postconditions: `playout_control_recover_total` incremented; idempotent within recovery window.
- `RequestTeardown(channel_id, reason)`
  - Preconditions: viewer count reaches 0 while channel is `Playing` or `Paused`.
  - Effects: initiates producer drain, keeps PaceController ticks active, records teardown start time.
  - Postconditions: `playout_control_teardown_duration_ms` logged once producer signals stopped; if timeout exceeded, escalates to forced stop and increments `playout_control_latency_violation_total`.
- `GetState(channel_id)` – Returns current control state with timestamps for auditing; side-effect free.

## Guarantees

- Control actions hit stated latency tolerances (see above) while preserving frame ordering.
- MasterClock alignment maintained across state transitions; no frame emitted earlier than its deadline.
- Commands processed serially per channel (`command_sequence` monotonic) to guarantee determinism under concurrent automation input.
- Seek operations flush stale frames and guarantee a deterministic resume PTS.
- Idempotency enforced via `(channel_id, command_id)` deduplication window of 60 s.

## Failure modes

- **Late seek**
  - Detection: `target_pts < current_pts`.
  - Metric/Alert: increments `playout_control_late_seek_total`; emits audit log.
  - Escalation: remains in current state; no transition to `Error`.
- **Mid-frame pause**
  - Detection: renderer cannot halt within one frame window.
  - Metric: `playout_control_pause_deviation_ms`.
  - Escalation: stays `Playing` until safe boundary; no `Error`.
- **External control timeout**
  - Detection: command not acknowledged within configured SLA.
  - Metric/Alert: `playout_control_timeout_ms` histogram; raises pager alert if >3 consecutive timeouts.
  - Escalation: transitions to `Error`; recovery requires `Recover`.
- **Back-pressure overflow**
  - Detection: command queue depth exceeds threshold for >100 ms.
  - Metric: `playout_control_queue_overflow_total`; emits warning log.
  - Escalation: remains in current state; auto-throttles requests.
- **Illegal transition attempt**
  - Detection: disallowed state change requested.
  - Metric: `playout_control_illegal_transition_total` with labels `{from,to}`.
  - Escalation: command rejected; state unchanged.
- **Teardown timeout**
  - Detection: producer fails to drain within configured teardown window.
  - Metric/Alert: increments `playout_control_latency_violation_total` and logs forced stop event.
  - Escalation: controller enters `Error` and forces producer shutdown; ChannelManager receives warning for manual follow-up.

## See also

- [MasterClock Domain](MasterClockDomain.md)
- [Metrics and Timing Domain](MetricsAndTimingDomain.md)
- [Playout Runtime](../runtime/PlayoutRuntime.md)
