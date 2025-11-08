# Phase 2 – Decode & Frame Bus Integration

> **Related:**  
> [Architecture Overview](../architecture/ArchitectureOverview.md)  
> [Playout Engine Contract](../contracts/PlayoutEngineContract.md)  
> [Phase 1 Complete](Phase1_Complete.md)

---

## ✨ Purpose

Phase 2 transforms the RetroVue Playout Engine from a stub RPC service into a real-time, production-ready media playout processor.  
This unlocks **real decoding**, **advanced buffering**, and **live telemetry**, allowing the Renderer to pull active streams directly from the native engine with clock-tight accuracy.

---

## 🎯 Objectives

| Subsystem                   | Goal                                             | Outcome                                             |
| --------------------------- | ------------------------------------------------ | --------------------------------------------------- |
| 🟦 **Decode Pipeline**      | Leverage `libavformat`/`libavcodec` for decoding | Real frame decoding from file/URI sources           |
| 🟧 **Frame Bus / Ring Buf** | Shared-memory frame queue                        | Thread-safe producer/consumer bridge to Renderer    |
| 🟨 **Telemetry / Metrics**  | Prometheus `/metrics` endpoint                   | Channel health, state, and debug visibility         |
| 🟩 **Integration Testing**  | Validate full Python ↔ C++ plumbing              | Ready for end-to-end and contract-driven tests (P3) |

---

## 🔬 Subsystem Details

### 1️⃣ Decode Pipeline

**Key Files:**

- `src/decode/FrameProducer.h`
- `src/decode/FrameProducer.cpp`

**Responsibilities:**

- Open media input using `avformat_open_input`.
- Select the optimal video stream and initialize decoders.
- Continuously decode frames, extracting **PTS**, **DTS**, and duration.
- Populate a `FrameMetadata` struct for every output frame:

  ```cpp
  struct FrameMetadata {
      int64_t pts;
      int64_t dts;
      double duration;
      std::string asset_uri;
  };
  ```

- **Thread Model:**
  - Dedicated decode thread per channel
  - Maintain a 2–3 frame lead time ahead of Renderer consumption
  - Must not block the main gRPC or metrics threads

---

### 2️⃣ Frame Bus / Ring Buffer

**Files:**

- `src/buffer/FrameRingBuffer.h`
- `src/buffer/FrameRingBuffer.cpp`

**Core Design:**

- Circular buffer of fixed size (e.g., 60 frames)
- Atomic read/write indices (`std::atomic<uint32_t>`)
- Non-blocking `push()`/`pop()` methods (returns success/failure)
- Optional: add a condition variable for underflow/overflow recovery in future phases

**Metrics & Telemetry:**

- Expose current buffer depth (`retrovue_playout_buffer_depth_frames`)
- Clearly report starvation (underflow) or overflow scenarios, e.g. via warning logs and Prometheus counters

---

### 3️⃣ Telemetry / Prometheus Metrics

**Files:**

- `src/telemetry/MetricsExporter.cpp`

**Responsibilities:**

- Serve Prometheus metrics endpoint at `/metrics`
- Export the following metrics:

  | Metric                                  | Type    | Description                                     |
  | --------------------------------------- | ------- | ----------------------------------------------- |
  | `retrovue_playout_channel_state`        | Gauge   | `ready`, `buffering`, `error`, or `stopped`     |
  | `retrovue_playout_buffer_depth_frames`  | Gauge   | Number of frames currently in buffer            |
  | `retrovue_playout_frame_gap_seconds`    | Gauge   | Deviation from MasterClock PTS alignment        |
  | `retrovue_playout_decode_failure_count` | Counter | Total decode errors (future: per channel/asset) |

---

### 4️⃣ Integration Testing

**New Tests:**

- `tests/test_decode.cpp`: verifies decode threads read frames and fill buffers as expected
- `tests/test_buffer.cpp`: stress-tests ring buffer push/pop, starvation, and overflow logic
- `scripts/test_playout_loop.py`: confirms the Python Renderer can consume live frames from the engine and that telemetry matches expectations

**Success Criteria:**

- ✅ `StartChannel` triggers correct decode thread lifecycle
- ✅ Frames flow continuously: **decoder → buffer → Renderer**
- ✅ All required metrics visible and accurately updating at `/metrics`
- ✅ No buffer underflow or overflow in steady-state playback
- ✅ All tests in `test_decode.cpp` and `test_buffer.cpp` pass reliably
- ✅ Integration script shows stable Python ↔ C++ playout handoff

---

**Bonus & Stretch Goals (recommended):**

- Add visibility logs for frame timing and buffer state to assist with e2e debugging.
- Make buffer size and decode thread count runtime-configurable.
- Ensure Prometheus `help`/`type` comments are correctly generated for all metrics.
- Consider stubbing out slate frame injection for error recovery (as prep for Phase 3).

---
