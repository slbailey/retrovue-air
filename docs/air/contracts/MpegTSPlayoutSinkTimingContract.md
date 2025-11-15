_Related: [MpegTSPlayoutSink Domain](../../domain/MpegTSPlayoutSinkDomain.md) • [Video File Producer Contract](../../contracts/VideoFileProducerDomainContract.md) • [Playout Engine Contract](../../contracts/PlayoutEngineContract.md) • [Architecture Overview](../../architecture/ArchitectureOverview.md)_

# Contract — MpegTSPlayoutSink Timing

Status: Enforced

## Purpose

This contract defines the **timing and scheduling guarantees** for `MpegTSPlayoutSink`:

- When it pulls frames from `FrameRingBuffer`
- How it uses `MasterClock` and frame PTS to schedule playout
- How it handles underruns, overruns, and late frames
- How it tracks timing-related statistics

This does **not** restate encoder/muxer correctness; those are covered by the main MpegTSPlayoutSink contract. This document only governs *when* frames should be sent.

---

## Scope

This contract applies to the following components:

- `MpegTSPlayoutSink` worker thread
- Its interaction with `FrameRingBuffer` and `MasterClock`
- Timing-related statistics and edge-case behavior

---

## Functional Expectations

### T-001: MasterClock Usage

**Rule**: The sink must use `MasterClock` as its only time source.

**Requirements**:

- All timing decisions use `master_clock_->now_utc_us()`
- The sink does not call `std::chrono::steady_clock::now()` or similar directly
- Sleep durations are derived from deltas between `now_utc_us` and `target_utc_us`

**Test idea**:

- Inject a fake MasterClock that returns controlled values
- Advance "time" by manipulating the fake clock and verify the sink reacts accordingly
- Verify there are no direct references to `std::chrono::steady_clock` in the sink timing loop

---

### T-002: PTS → Station Time Mapping

**Rule**: The sink must map frame PTS (µs) into station time using an affine transform anchored on the first frame.

**Behavior**:

- On the first frame popped for playout, compute:

  ```cpp
  pts_zero_utc_us = master_clock_->now_utc_us() - frame.metadata.pts
  ```

- For every frame:

  ```cpp
  target_utc_us = pts_zero_utc_us + frame.metadata.pts
  ```

- PTS must be monotonically increasing for frames from the same asset

**Test idea**:

- Use a fake MasterClock starting at 1,000,000 µs
- Feed three frames with PTS = 0, 33_333, 66_666
- Capture the internal `target_utc_us` values (exposed via a test hook or debug API)
- Verify they equal 1,000,000, 1,033,333, 1,066,666 respectively

---

### T-003: On-Time and Early Frame Handling

**Rule**: If `now_utc_us <= target_utc_us`, the sink must wait until (or very close to) `target_utc_us` before encoding/sending the frame.

**Behavior**:

- When a frame is loaded and its `target_utc_us` is in the future:
  - The worker sleeps until `target_utc_us` (minus a small safety margin, if needed)
  - It then encodes and muxes the frame

**Test idea**:

- Use a fake MasterClock and a test sink that records `(frame.pts, send_time)`
- Feed a buffer with frames at 30 fps PTS spacing
- Drive the clock forward in small increments
- Assert that `send_time` for each frame is within `[target_utc_us - epsilon, target_utc_us + epsilon]`

---

### T-004: Underrun Handling (Empty Buffer)

**Rule**: When the buffer is empty at the time a frame is needed, the sink must record an underrun and back off.

**Behavior**:

- If `FrameRingBuffer::Pop()` returns `false` when the sink needs a frame:
  - Increment `stats.buffer_underruns`
  - Sleep for a short backoff duration (e.g. 2–5 ms)
  - Retry popping a frame
- The sink does not synthesize frames or block indefinitely

**Test idea**:

- Create a sink with an empty `FrameRingBuffer`
- Drive MasterClock forward to simulate real-time
- After a brief run:
  - Verify `stats.buffer_underruns > 0`
  - Verify the worker thread is still alive (no crash, no tight busy loop)

---

### T-005: Late Frame Detection and Drops

**Rule**: Frames that are "too late" must be dropped and counted as late drops.

**Behavior**:

- For a frame with `target_utc_us`, compute:

  ```cpp
  lateness = now_utc_us - target_utc_us
  ```

- If `lateness <= 0`: treat as on-time/early (see T-003)
- If `0 < lateness <= kMaxLateToleranceUs`: encode immediately (slightly late)
- If `lateness > kMaxLateToleranceUs`:
  - Drop the frame
  - Increment `stats.late_frame_drops`
  - Pop the next frame and evaluate again

**Test idea**:

- Use a fake MasterClock that jumps far ahead (e.g., simulating a stall)
- Provide several frames with PTS spaced at 33 ms
- Force `now_utc_us` to be late by >2 frame durations
- Verify that the first one or more frames are dropped and counted as late drops
- Verify the sink eventually resumes encoding once it catches up

---

### T-006: Monotonic Output Order

**Rule**: The sink must never send frames out of PTS order.

**Behavior**:

- Even when dropping late frames, any frame that is actually encoded must have PTS ≥ the last encoded frame's PTS

**Test idea**:

- Feed frames with increasing PTS into the buffer
- Simulate lateness so some frames are dropped
- Record PTS values of encoded frames via a test hook
- Assert the sequence of encoded PTS is strictly increasing

---

### T-007: Graceful Stop and Timing Loop Exit

**Rule**: When `Stop()` is requested, the timing loop must exit cleanly.

**Behavior**:

- `Stop()` or `RequestStop()` sets a stop flag
- The worker loop checks this flag at least once per iteration
- The thread exits in a bounded amount of time (no infinite waits on timing logic)
- No further frames are popped or encoded after stop

**Test idea**:

- Start the sink with a populated buffer and fake MasterClock
- Let it encode a few frames
- Call `Stop()`
- Join the thread and assert it returns in <N ms
- Verify no additional "send" events occur after `Stop()`

---

## Test Coverage Requirements

Timing tests should live alongside existing sink contract tests, for example:

`tests/contracts/mpegts_sink/MpegTSPlayoutSinkTimingTests.cpp`

**Coverage**:

- T-001 through T-007 must have at least one dedicated test case each
- Tests should use a fake MasterClock and, where needed, a test sink subclass (or hooks) to observe:
  - When frames are sent
  - Which frames are dropped
  - Statistics (`buffer_underruns`, `late_frame_drops`)
- Tests must be deterministic and complete in < 2 seconds

---

## See Also

- [MpegTSPlayoutSink Domain](../../domain/MpegTSPlayoutSinkDomain.md) — Domain model and architecture
- [Video File Producer Contract](../../contracts/VideoFileProducerDomainContract.md) — Producer timing contract
- [Playout Engine Contract](../../contracts/PlayoutEngineContract.md) — Overall playout engine contract
- [Architecture Overview](../../architecture/ArchitectureOverview.md) — System-wide architecture





