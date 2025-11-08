_Related: [Renderer Domain](../domain/RendererDomain.md) • [Playout Engine Domain](../domain/PlayoutEngineDomain.md) • [Metrics and Timing Contract](MetricsAndTimingDomainContract.md) • [Architecture Overview](../architecture/ArchitectureOverview.md) • [Phase 2 Plan](../milestones/Phase2_Plan.md)_

# Contract — Renderer Subsystem

Status: Enforced

## Purpose

This document defines the **behavioral and testing contract** for the Renderer subsystem in the RetroVue Playout Engine. The Renderer is responsible for consuming decoded frames from the FrameRingBuffer and producing visual output through one or more rendering backends (headless, preview window, MPEG-TS muxing).

This contract establishes:

- **Functional guarantees** for frame consumption timing, buffer management, and mode transitions
- **Performance expectations** for throughput, latency, and jitter
- **Error recovery procedures** for malformed frames and initialization failures
- **Integration requirements** with MetricsHTTPServer for telemetry validation
- **Verification criteria** for automated testing and continuous validation

The Renderer subsystem must operate deterministically, maintain clock alignment with the MasterClock, and provide observable telemetry for all operational states.

---

## Scope

This contract enforces guarantees for the following Renderer implementations:

### 1. FrameRenderer (Abstract Base)

**Purpose**: Defines the common interface for all renderer implementations.

**Contract**:

```cpp
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;
    
    // Pull next frame from channel buffer
    virtual bool PullFrame(int channel_id, Frame& out_frame) = 0;
    
    // Check if renderer is ready to accept frames
    virtual bool IsReady() const = 0;
    
    // Get current frame consumption rate (frames/second)
    virtual double GetFps() const = 0;
    
    // Graceful shutdown
    virtual void Shutdown() = 0;
};
```

**Guarantees**:

- All implementations must be thread-safe for single-threaded consumption
- `PullFrame()` must not block for more than 1ms
- `IsReady()` must reflect actual operational state
- `Shutdown()` must complete within 200ms

---

### 2. HeadlessRenderer

**Purpose**: Consumes frames without producing visual output. Used for testing, benchmarking, and headless server deployments.

**Behavior**:

- Pulls frames at configured rate (default: 30 fps)
- Validates frame metadata (PTS monotonicity, dimension consistency)
- Emits telemetry metrics
- No SDL2 or display dependencies

**Use Cases**:

- Automated integration tests
- Performance benchmarking
- Server deployments without display hardware

---

### 3. PreviewRenderer

**Purpose**: Displays decoded frames in an SDL2-powered debug window with real-time statistics overlay.

**Behavior**:

- Initializes SDL2 window with configurable resolution
- Renders YUV420 frames using GPU acceleration (if available)
- Overlays real-time metrics (FPS, PTS gap, buffer depth)
- Supports window resize and close events
- Falls back to headless mode if SDL2 initialization fails

**Use Cases**:

- Operator preview during development
- Visual validation of playout output
- Real-time debugging of timing issues

---

## Test Environment Setup

All Renderer contract tests must run in a controlled environment with the following prerequisites:

### Required Resources

| Resource              | Specification                                      | Purpose                            |
| --------------------- | -------------------------------------------------- | ---------------------------------- |
| Test Video Asset      | H.264 1080p30, 10s duration, monotonic PTS         | Standard frame source for testing  |
| FrameRingBuffer       | 60-frame capacity, pre-populated with 45 frames    | Simulates steady-state buffer      |
| MasterClock Mock      | Monotonic, microsecond precision                   | Controlled timing source           |
| MetricsHTTPServer     | Running on `localhost:9308`                        | Telemetry verification             |
| SDL2 (optional)       | v2.0.20+ with video subsystem enabled              | Preview renderer validation        |

### Environment Variables

```bash
RETROVUE_RENDERER_MODE=headless        # Default: headless
RETROVUE_PREVIEW_WIDTH=1280            # Preview window width
RETROVUE_PREVIEW_HEIGHT=720            # Preview window height
RETROVUE_RENDERER_TARGET_FPS=30        # Frame consumption rate
RETROVUE_METRICS_ENABLED=true          # Enable telemetry export
```

### Pre-Test Validation

Before running contract tests, verify:

1. ✅ Test video asset decodes successfully (no corrupt frames)
2. ✅ FrameRingBuffer operations pass smoke tests
3. ✅ MasterClock advances monotonically
4. ✅ Metrics endpoint responds at `/metrics`
5. ✅ SDL2 available (for preview tests) or skipped gracefully

---

## Functional Expectations

The Renderer subsystem must satisfy the following behavioral guarantees:

### FE-001: Frame Consumption Timing

**Rule**: Frames must be consumed relative to their PTS metadata, aligned with MasterClock.

**Expected Behavior**:

- Renderer queries `MasterClock::now_utc_us()` before each pull
- If `current_time >= frame.metadata.pts`, frame is consumed immediately
- If `current_time < frame.metadata.pts`, renderer waits until scheduled time
- Waiting must use non-blocking sleep (yield CPU)

**Validation**:

```cpp
// Pseudo-test
Frame frame;
frame.metadata.pts = clock.now_utc_us() + 33333; // +33ms (1 frame @ 30fps)

renderer.PullFrame(1, frame);
int64_t actual_render_time = clock.now_utc_us();

ASSERT_NEAR(actual_render_time, frame.metadata.pts, 2000); // ±2ms tolerance
```

**Failure Modes**:

- ❌ Frame rendered too early (ahead of PTS) → timing drift
- ❌ Frame rendered too late (> 16ms behind PTS) → visible stutter

---

### FE-002: Empty Buffer Handling

**Rule**: Renderer must handle empty buffer gracefully without hanging or crashing.

**Expected Behavior**:

- `PullFrame()` returns `false` if buffer is empty
- Renderer emits `buffer_underrun` metric event
- Renderer continues polling at reduced rate (5 Hz) until frame available
- No busy-waiting (CPU must yield)

**Validation**:

```cpp
FrameRingBuffer buffer(60);
// buffer is empty

HeadlessRenderer renderer(buffer);
Frame frame;

bool result = renderer.PullFrame(1, frame);

ASSERT_FALSE(result);
ASSERT_EQ(metrics.GetCounter("renderer_underrun_total"), 1);
```

**Failure Modes**:

- ❌ Infinite loop / busy wait consuming 100% CPU
- ❌ Crash on null frame access
- ❌ Metrics not updated

---

### FE-003: Mode Transition (Headless ↔ Preview)

**Rule**: Renderer must support hot-swap between headless and preview modes without frame loss.

**Sequence**:

1. Start in headless mode
2. Consume 100 frames
3. Switch to preview mode (`EnablePreview()`)
4. SDL2 window opens within 500ms
5. Continue consuming frames without gap
6. Switch back to headless mode (`DisablePreview()`)
7. SDL2 window closes within 200ms
8. Verify no frames dropped during transitions

**Validation**:

```cpp
HeadlessRenderer renderer(buffer);

// Phase 1: Headless
for (int i = 0; i < 100; i++) {
    Frame f;
    ASSERT_TRUE(renderer.PullFrame(1, f));
}

// Phase 2: Enable preview
renderer.EnablePreview();
std::this_thread::sleep_for(std::chrono::milliseconds(500));
ASSERT_TRUE(renderer.HasActiveWindow());

// Phase 3: Continue consuming
for (int i = 0; i < 100; i++) {
    Frame f;
    ASSERT_TRUE(renderer.PullFrame(1, f));
}

// Phase 4: Disable preview
renderer.DisablePreview();
std::this_thread::sleep_for(std::chrono::milliseconds(200));
ASSERT_FALSE(renderer.HasActiveWindow());

// Verify metrics
ASSERT_EQ(metrics.GetCounter("frames_rendered_total"), 200);
ASSERT_EQ(metrics.GetCounter("frames_dropped_total"), 0);
```

**Failure Modes**:

- ❌ SDL2 window creation blocks frame consumption
- ❌ Frames dropped during mode transition
- ❌ Window fails to close (resource leak)

---

### FE-004: PTS Monotonicity Validation

**Rule**: Renderer must validate that consumed frames have monotonically increasing PTS.

**Expected Behavior**:

- On each `PullFrame()`, compare `frame.metadata.pts` to previous frame PTS
- If PTS decreases or duplicates, log error and increment `renderer_pts_violation_total`
- Continue consuming (skip invalid frame)
- Do NOT crash or stop rendering

**Validation**:

```cpp
FrameRingBuffer buffer(60);

// Push frames with non-monotonic PTS
buffer.Push(CreateFrame(pts = 1000));
buffer.Push(CreateFrame(pts = 2000));
buffer.Push(CreateFrame(pts = 1500)); // ❌ Violation
buffer.Push(CreateFrame(pts = 3000));

HeadlessRenderer renderer(buffer);

Frame f1, f2, f3, f4;
ASSERT_TRUE(renderer.PullFrame(1, f1)); // pts=1000
ASSERT_TRUE(renderer.PullFrame(1, f2)); // pts=2000
ASSERT_TRUE(renderer.PullFrame(1, f3)); // pts=1500 → error logged
ASSERT_TRUE(renderer.PullFrame(1, f4)); // pts=3000

ASSERT_EQ(metrics.GetCounter("renderer_pts_violation_total"), 1);
```

**Failure Modes**:

- ❌ Renderer crashes on PTS violation
- ❌ Violation not logged or counted
- ❌ Subsequent frames rejected incorrectly

---

### FE-005: Frame Dimension Consistency

**Rule**: All frames consumed by a single channel must have consistent dimensions.

**Expected Behavior**:

- Renderer captures first frame's width/height as baseline
- Subsequent frames with mismatched dimensions trigger error
- Increment `renderer_dimension_mismatch_total`
- Skip mismatched frame, continue rendering

**Validation**:

```cpp
FrameRingBuffer buffer(60);

buffer.Push(CreateFrame(width=1920, height=1080));
buffer.Push(CreateFrame(width=1920, height=1080));
buffer.Push(CreateFrame(width=1280, height=720)); // ❌ Mismatch
buffer.Push(CreateFrame(width=1920, height=1080));

HeadlessRenderer renderer(buffer);

for (int i = 0; i < 4; i++) {
    Frame f;
    renderer.PullFrame(1, f);
}

ASSERT_EQ(metrics.GetCounter("renderer_dimension_mismatch_total"), 1);
ASSERT_EQ(metrics.GetCounter("frames_rendered_total"), 3); // 1 skipped
```

**Failure Modes**:

- ❌ Dimension mismatch causes crash
- ❌ Preview window renders corrupted frame
- ❌ Metrics not updated

---

## Performance Metrics

The Renderer subsystem must meet the following performance criteria:

### PM-001: Render Throughput (Headless)

**Target**: ≥ 30 fps sustained over 60 seconds

**Measurement**:

```cpp
HeadlessRenderer renderer(buffer);
auto start = std::chrono::steady_clock::now();

int frame_count = 0;
while (std::chrono::steady_clock::now() - start < 60s) {
    Frame f;
    if (renderer.PullFrame(1, f)) {
        frame_count++;
    }
}

double fps = frame_count / 60.0;
ASSERT_GE(fps, 30.0);
```

**Success Criteria**:

- ✅ Minimum: 30 fps (1800 frames in 60 seconds)
- ✅ Target: 60 fps (3600 frames in 60 seconds)
- ✅ CPU usage: ≤ 10% of single core

**Failure Conditions**:

- ❌ FPS < 30 → insufficient throughput
- ❌ CPU usage > 50% → inefficient polling
- ❌ Frame drops > 0 → buffer management issue

---

### PM-002: Frame Consumption Jitter

**Target**: ≤ ±2 frames over 60 seconds

**Definition**: Jitter = standard deviation of frame-to-frame interval

**Measurement**:

```cpp
std::vector<int64_t> intervals;
int64_t prev_pts = 0;

for (int i = 0; i < 1800; i++) {
    Frame f;
    renderer.PullFrame(1, f);
    
    if (prev_pts > 0) {
        intervals.push_back(f.metadata.pts - prev_pts);
    }
    prev_pts = f.metadata.pts;
}

double mean_interval = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
double variance = 0.0;
for (auto interval : intervals) {
    variance += std::pow(interval - mean_interval, 2);
}
double std_dev = std::sqrt(variance / intervals.size());

// Convert microseconds to frames (@ 30fps, 33333 µs/frame)
double jitter_frames = std_dev / 33333.0;

ASSERT_LE(jitter_frames, 2.0);
```

**Success Criteria**:

- ✅ Jitter ≤ 1 frame: Excellent
- ✅ Jitter ≤ 2 frames: Acceptable
- ⚠️ Jitter > 2 frames: Marginal (investigate)
- ❌ Jitter > 5 frames: Unacceptable

---

### PM-003: Frame Latency

**Target**: ≤ 16 ms average (time from buffer pop to render complete)

**Measurement**:

```cpp
std::vector<int64_t> latencies;

for (int i = 0; i < 1000; i++) {
    Frame f;
    
    auto start = std::chrono::high_resolution_clock::now();
    renderer.PullFrame(1, f);
    auto end = std::chrono::high_resolution_clock::now();
    
    int64_t latency_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    latencies.push_back(latency_us);
}

double avg_latency_ms = std::accumulate(latencies.begin(), latencies.end(), 0.0) / latencies.size() / 1000.0;

ASSERT_LE(avg_latency_ms, 16.0);
```

**Success Criteria**:

| Percentile | Target Latency | Description        |
| ---------- | -------------- | ------------------ |
| p50        | ≤ 8 ms         | Median             |
| p95        | ≤ 16 ms        | 95th percentile    |
| p99        | ≤ 32 ms        | 99th percentile    |
| p100 (max) | ≤ 50 ms        | Worst-case latency |

**Failure Conditions**:

- ❌ p95 > 16 ms → SDL2 vsync blocking
- ❌ p99 > 32 ms → memory allocation in hot path
- ❌ p100 > 50 ms → system scheduling issue

---

### PM-004: Preview Renderer Overhead

**Target**: Preview mode adds ≤ 20% CPU vs. headless mode

**Measurement**:

```cpp
// Phase 1: Headless
HeadlessRenderer headless(buffer);
auto cpu_start = GetProcessCpuTime();
RenderFrames(headless, 1000);
auto cpu_headless = GetProcessCpuTime() - cpu_start;

// Phase 2: Preview
PreviewRenderer preview(buffer);
cpu_start = GetProcessCpuTime();
RenderFrames(preview, 1000);
auto cpu_preview = GetProcessCpuTime() - cpu_start;

double overhead = (cpu_preview - cpu_headless) / cpu_headless;
ASSERT_LE(overhead, 0.20); // 20% increase
```

**Success Criteria**:

- ✅ Overhead ≤ 10%: Excellent (GPU-accelerated)
- ✅ Overhead ≤ 20%: Acceptable (software rendering)
- ⚠️ Overhead > 20%: Investigate inefficiency
- ❌ Overhead > 50%: Unacceptable

---

## Shutdown Behavior

The Renderer must terminate cleanly under all conditions.

### SH-001: Graceful Shutdown

**Sequence**:

1. Call `renderer.Shutdown()`
2. Renderer stops pulling frames
3. Renderer flushes any pending frames to output
4. SDL2 window (if active) closes
5. Metrics final snapshot emitted
6. Destructor completes without crash

**Timing Constraints**:

| Phase                  | Maximum Duration | Failure Action              |
| ---------------------- | ---------------- | --------------------------- |
| Stop frame pull        | 50 ms            | Force-stop consumption loop |
| Flush pending frames   | 100 ms           | Discard remaining frames    |
| Close SDL2 window      | 50 ms            | Force-destroy window        |
| Emit final metrics     | 10 ms            | Skip metrics export         |
| **Total Shutdown Time** | **200 ms**       | Hard timeout kill           |

**Validation**:

```cpp
PreviewRenderer renderer(buffer);

// Render for 5 seconds
auto render_start = std::chrono::steady_clock::now();
while (std::chrono::steady_clock::now() - render_start < 5s) {
    Frame f;
    renderer.PullFrame(1, f);
}

// Shutdown
auto shutdown_start = std::chrono::steady_clock::now();
renderer.Shutdown();
auto shutdown_duration = std::chrono::steady_clock::now() - shutdown_start;

ASSERT_LE(shutdown_duration, std::chrono::milliseconds(200));
ASSERT_FALSE(renderer.HasActiveWindow());
ASSERT_TRUE(metrics.HasSnapshot("renderer_final_state"));
```

**Failure Modes**:

- ❌ Shutdown hangs (SDL2 deadlock)
- ❌ Crash in destructor
- ❌ Metrics not flushed
- ❌ Memory leak (valgrind failure)

---

### SH-002: Abort During Underrun

**Scenario**: Renderer is waiting for frames (underrun state) when shutdown is called.

**Expected Behavior**:

- Shutdown interrupts wait loop immediately
- No frame flush needed (buffer empty)
- Completion within 100 ms

**Validation**:

```cpp
FrameRingBuffer buffer(60); // Empty buffer
HeadlessRenderer renderer(buffer);

// Start consuming (will underrun)
std::thread consume_thread([&]() {
    Frame f;
    while (renderer.PullFrame(1, f)) {}
});

std::this_thread::sleep_for(std::chrono::milliseconds(500));

// Shutdown while underrun
auto start = std::chrono::steady_clock::now();
renderer.Shutdown();
auto duration = std::chrono::steady_clock::now() - start;

ASSERT_LE(duration, std::chrono::milliseconds(100));

consume_thread.join();
```

---

### SH-003: Shutdown During Mode Transition

**Scenario**: Shutdown called while switching from headless to preview mode.

**Expected Behavior**:

- Mode transition aborted cleanly
- SDL2 initialization cancelled if in-progress
- No resource leaks
- Completion within 200 ms

**Validation**:

```cpp
HeadlessRenderer renderer(buffer);

// Trigger mode transition
std::thread transition_thread([&]() {
    renderer.EnablePreview();
});

// Shutdown during transition
std::this_thread::sleep_for(std::chrono::milliseconds(100));
renderer.Shutdown();

transition_thread.join();

ASSERT_FALSE(renderer.HasActiveWindow());
ASSERT_EQ(CheckMemoryLeaks(), 0);
```

---

## Error Handling

The Renderer must recover gracefully from all error conditions.

### EH-001: Malformed Frame Data

**Error**: Frame data buffer is null or size is zero.

**Recovery Action**:

1. Log error: `"Renderer: Invalid frame data for channel {id}"`
2. Increment `renderer_invalid_frame_total`
3. Skip frame, continue to next
4. Emit metrics event

**Validation**:

```cpp
FrameRingBuffer buffer(60);

Frame bad_frame;
bad_frame.metadata.pts = 1000;
bad_frame.data.clear(); // ❌ Empty data
bad_frame.width = 1920;
bad_frame.height = 1080;

buffer.Push(bad_frame);
buffer.Push(CreateValidFrame(pts = 2000));

HeadlessRenderer renderer(buffer);

Frame f1, f2;
ASSERT_TRUE(renderer.PullFrame(1, f1)); // Skips bad frame
ASSERT_EQ(f1.metadata.pts, 2000);
ASSERT_EQ(metrics.GetCounter("renderer_invalid_frame_total"), 1);
```

**Failure Modes**:

- ❌ Crash on null pointer dereference
- ❌ Infinite loop retrying bad frame
- ❌ Metrics not updated

---

### EH-002: SDL2 Initialization Failure

**Error**: SDL2 cannot initialize video subsystem (missing display, driver issue).

**Recovery Action**:

1. Log warning: `"Preview mode unavailable: SDL2 init failed"`
2. Fall back to headless mode
3. Set `renderer_preview_available = 0` metric
4. Continue normal operation

**Validation**:

```bash
# Force SDL2 failure
export SDL_VIDEODRIVER=invalid

# Run preview renderer
PreviewRenderer renderer(buffer);

ASSERT_FALSE(renderer.HasActiveWindow());
ASSERT_TRUE(renderer.IsInHeadlessMode());
ASSERT_EQ(metrics.GetGauge("renderer_preview_available"), 0);

# Verify frames still consumed
Frame f;
ASSERT_TRUE(renderer.PullFrame(1, f));
```

**Failure Modes**:

- ❌ Crash on SDL2 init failure
- ❌ Renderer stops consuming frames
- ❌ No fallback to headless mode

---

### EH-003: Window Close Event

**Error**: User closes preview window via OS controls.

**Recovery Action**:

1. Detect SDL_QUIT event
2. Log info: `"Preview window closed by user"`
3. Transition to headless mode
4. Continue frame consumption
5. Increment `renderer_window_closed_by_user_total`

**Validation**:

```cpp
PreviewRenderer renderer(buffer);
ASSERT_TRUE(renderer.HasActiveWindow());

// Simulate user closing window
SDL_Event quit_event;
quit_event.type = SDL_QUIT;
SDL_PushEvent(&quit_event);

// Renderer processes event on next frame
Frame f;
renderer.PullFrame(1, f);

ASSERT_FALSE(renderer.HasActiveWindow());
ASSERT_TRUE(renderer.IsInHeadlessMode());
ASSERT_EQ(metrics.GetCounter("renderer_window_closed_by_user_total"), 1);
```

**Failure Modes**:

- ❌ Renderer crashes on window close
- ❌ Frame consumption stops
- ❌ Metrics not updated

---

### EH-004: Clock Synchronization Failure

**Error**: MasterClock returns invalid time (zero, negative, or time-travel).

**Recovery Action**:

1. Log error: `"Renderer: MasterClock returned invalid time"`
2. Increment `renderer_clock_error_total`
3. Fall back to system time for this frame only
4. Continue rendering

**Validation**:

```cpp
MockMasterClock clock;
clock.SetReturnValue(-1000); // ❌ Negative time

HeadlessRenderer renderer(buffer, clock);

Frame f;
ASSERT_TRUE(renderer.PullFrame(1, f)); // Should not crash

ASSERT_EQ(metrics.GetCounter("renderer_clock_error_total"), 1);
```

**Failure Modes**:

- ❌ Crash on invalid time
- ❌ Frame rendered with incorrect timing
- ❌ Metrics not updated

---

## Integration Tests

The Renderer must be tested in combination with other subsystems.

### IT-001: Renderer + MetricsHTTPServer

**Purpose**: Verify renderer metrics are correctly exposed via Prometheus endpoint.

**Test Procedure**:

1. Start MetricsHTTPServer on `localhost:9308`
2. Start HeadlessRenderer
3. Render 1000 frames
4. Query `/metrics` endpoint
5. Validate metric values

**Expected Metrics**:

```prometheus
# Renderer throughput
retrovue_renderer_frames_rendered_total{channel="1"} 1000

# Renderer FPS
retrovue_renderer_fps{channel="1"} 30.0

# Frame timing
retrovue_renderer_frame_delay_ms{channel="1"} 0.5

# Error counters
retrovue_renderer_underrun_total{channel="1"} 0
retrovue_renderer_invalid_frame_total{channel="1"} 0
retrovue_renderer_pts_violation_total{channel="1"} 0

# Mode indicator
retrovue_renderer_preview_active{channel="1"} 0
```

**Assertions**:

```cpp
auto metrics = FetchMetrics("http://localhost:9308/metrics");

ASSERT_EQ(metrics["retrovue_renderer_frames_rendered_total"], 1000);
ASSERT_NEAR(metrics["retrovue_renderer_fps"], 30.0, 0.5);
ASSERT_LE(metrics["retrovue_renderer_frame_delay_ms"], 2.0);
ASSERT_EQ(metrics["retrovue_renderer_underrun_total"], 0);
```

---

### IT-002: Renderer + FrameProducer

**Purpose**: Verify renderer can consume frames at decode rate without underruns.

**Test Procedure**:

1. Start FrameProducer decoding test video (30 fps)
2. Start HeadlessRenderer consuming from shared buffer
3. Run for 60 seconds
4. Validate no underruns or frame drops

**Success Criteria**:

```cpp
FrameRingBuffer buffer(60);
FrameProducer producer(buffer, "test_video.mp4");
HeadlessRenderer renderer(buffer);

// Start producer
producer.Start();

// Consume for 60 seconds
auto start = std::chrono::steady_clock::now();
int frame_count = 0;

while (std::chrono::steady_clock::now() - start < 60s) {
    Frame f;
    if (renderer.PullFrame(1, f)) {
        frame_count++;
    }
}

producer.Stop();

ASSERT_EQ(metrics.GetCounter("renderer_underrun_total"), 0);
ASSERT_EQ(metrics.GetCounter("frames_dropped_total"), 0);
ASSERT_GE(frame_count, 1800); // 30 fps × 60s
ASSERT_GE(metrics.GetGauge("buffer_depth_frames"), 30); // Healthy buffer
```

---

### IT-003: Multi-Channel Rendering

**Purpose**: Verify renderer can handle multiple channels simultaneously.

**Test Procedure**:

1. Create 4 channels with independent buffers
2. Start HeadlessRenderer consuming from all channels
3. Render 1000 frames per channel
4. Validate correct frame routing and metrics

**Success Criteria**:

```cpp
std::vector<FrameRingBuffer> buffers(4, FrameRingBuffer(60));
HeadlessRenderer renderer;

// Populate buffers
for (int ch = 0; ch < 4; ch++) {
    for (int i = 0; i < 1000; i++) {
        buffers[ch].Push(CreateFrame(pts = i * 33333, channel = ch));
    }
}

// Render all channels
for (int i = 0; i < 1000; i++) {
    for (int ch = 0; ch < 4; ch++) {
        Frame f;
        ASSERT_TRUE(renderer.PullFrame(ch, f));
        ASSERT_EQ(f.metadata.channel_id, ch); // Verify correct routing
    }
}

// Validate metrics per channel
for (int ch = 0; ch < 4; ch++) {
    auto label = fmt::format("channel=\"{}\"", ch);
    ASSERT_EQ(metrics.GetCounter("frames_rendered_total", label), 1000);
}
```

---

### IT-004: Renderer + Clock Drift Handling

**Purpose**: Verify renderer compensates for clock drift.

**Test Procedure**:

1. Configure MasterClock with artificial +500ms drift
2. Render 100 frames
3. Validate renderer adjusts timing to maintain frame rate
4. Check `frame_delay_ms` metric reflects drift

**Success Criteria**:

```cpp
MockMasterClock clock;
clock.SetDrift(500000); // +500ms

HeadlessRenderer renderer(buffer, clock);

auto start = std::chrono::steady_clock::now();
for (int i = 0; i < 100; i++) {
    Frame f;
    renderer.PullFrame(1, f);
}
auto duration = std::chrono::steady_clock::now() - start;

// Should take ~3.33s (100 frames @ 30fps), despite clock drift
ASSERT_NEAR(duration.count() / 1000.0, 3333.0, 100.0); // ±100ms tolerance

// Metric should reflect drift
ASSERT_NEAR(metrics.GetGauge("renderer_frame_delay_ms"), 500.0, 10.0);
```

---

## Verification Criteria Table

The following table defines test IDs, purposes, expected results, and success metrics for all contract tests.

| Test ID        | Category          | Purpose                                              | Expected Result                            | Success Metric                              |
| -------------- | ----------------- | ---------------------------------------------------- | ------------------------------------------ | ------------------------------------------- |
| **FE-001-T01** | Functional        | Validate frame consumed at correct PTS               | Frame rendered within ±2ms of PTS          | `abs(render_time - pts) ≤ 2000 µs`          |
| **FE-002-T01** | Functional        | Verify empty buffer returns false                    | `PullFrame()` returns false                | `result == false`                           |
| **FE-002-T02** | Functional        | Verify underrun metric incremented                   | Underrun counter incremented               | `renderer_underrun_total == 1`              |
| **FE-003-T01** | Functional        | Hot-swap headless → preview                          | Preview window opens, no frame loss        | `frames_dropped == 0`, window active        |
| **FE-003-T02** | Functional        | Hot-swap preview → headless                          | Preview window closes, no frame loss       | `frames_dropped == 0`, window closed        |
| **FE-004-T01** | Functional        | Detect non-monotonic PTS                             | Error logged, frame skipped                | `renderer_pts_violation_total == 1`         |
| **FE-005-T01** | Functional        | Detect dimension mismatch                            | Error logged, frame skipped                | `renderer_dimension_mismatch_total == 1`    |
| **PM-001-T01** | Performance       | Measure headless throughput                          | ≥ 30 fps sustained for 60s                 | `fps ≥ 30.0`                                |
| **PM-002-T01** | Performance       | Measure frame consumption jitter                     | ≤ ±2 frames over 60s                       | `std_dev(intervals) ≤ 66666 µs`             |
| **PM-003-T01** | Performance       | Measure frame latency (p95)                          | ≤ 16 ms                                    | `latency_p95 ≤ 16000 µs`                    |
| **PM-004-T01** | Performance       | Measure preview mode overhead                        | ≤ 20% CPU vs headless                      | `(cpu_preview - cpu_headless) / cpu_headless ≤ 0.20` |
| **SH-001-T01** | Shutdown          | Verify graceful shutdown completes                   | Shutdown within 200ms                      | `shutdown_duration ≤ 200 ms`                |
| **SH-002-T01** | Shutdown          | Verify shutdown during underrun                      | Shutdown within 100ms                      | `shutdown_duration ≤ 100 ms`                |
| **SH-003-T01** | Shutdown          | Verify shutdown during mode transition               | Shutdown within 200ms, no leaks            | `shutdown_duration ≤ 200 ms`, valgrind clean|
| **EH-001-T01** | Error Handling    | Handle null frame data                               | Frame skipped, counter incremented         | `renderer_invalid_frame_total == 1`         |
| **EH-002-T01** | Error Handling    | Handle SDL2 init failure                             | Fallback to headless, no crash             | `preview_available == 0`, no crash          |
| **EH-003-T01** | Error Handling    | Handle window close event                            | Transition to headless, continue rendering | `window_closed_by_user_total == 1`          |
| **EH-004-T01** | Error Handling    | Handle invalid MasterClock time                      | Error logged, fallback to system time      | `renderer_clock_error_total == 1`           |
| **IT-001-T01** | Integration       | Validate metrics export                              | All metrics present with correct values    | HTTP 200, all metrics match expected        |
| **IT-002-T01** | Integration       | Validate renderer + producer sync                    | No underruns over 60s                      | `renderer_underrun_total == 0`              |
| **IT-003-T01** | Integration       | Validate multi-channel rendering                     | Correct frame routing per channel          | All channel metrics match expected          |
| **IT-004-T01** | Integration       | Validate clock drift compensation                    | Frame rate maintained despite drift        | `fps ≈ 30.0`, `frame_delay_ms ≈ drift`      |

---

## Test Execution Standards

### Automated Test Suite

All contract tests must be executable via:

```bash
# Run all renderer contract tests
ctest -R "^Renderer" -VV

# Run specific test category
ctest -R "^Renderer.Functional" -VV
ctest -R "^Renderer.Performance" -VV
ctest -R "^Renderer.Integration" -VV
```

### CI/CD Requirements

- ✅ All contract tests pass on every commit
- ✅ Performance tests pass on release branches
- ✅ Integration tests pass on multi-channel configurations
- ✅ Valgrind clean (no memory leaks)
- ✅ ThreadSanitizer clean (no data races)

### Test Environment Matrix

| Platform       | SDL2 Mode | Test Coverage              |
| -------------- | --------- | -------------------------- |
| Linux x64      | Headless  | Full suite                 |
| Linux x64      | Preview   | Full suite                 |
| macOS ARM64    | Headless  | Full suite                 |
| macOS ARM64    | Preview   | Full suite                 |
| Windows x64    | Headless  | Full suite                 |
| Windows x64    | Preview   | Full suite (GUI required)  |
| Docker (CI)    | Headless  | Functional + Performance   |

---

## See Also

- [Renderer Domain](../domain/RendererDomain.md) — Full renderer domain specification
- [Playout Engine Domain](../domain/PlayoutEngineDomain.md) — Full playout engine domain model and guarantees
- [Metrics and Timing Contract](MetricsAndTimingDomainContract.md) — Time synchronization and telemetry contracts
- [Architecture Overview](../architecture/ArchitectureOverview.md) — System context and component integration
- [FrameRingBuffer Header](../../include/retrovue/buffer/FrameRingBuffer.h) — Buffer implementation details
- [Development Standards](../developer/DevelopmentStandards.md) — C++ project structure and conventions
- [Phase 2 Plan](../milestones/Phase2_Plan.md) — Implementation milestones and deliverables
- [RetroVue MasterClock](../../../Retrovue/docs/domain/MasterClock.md) — Time synchronization contract

---

## Changelog

| Date       | Version | Changes                                         |
| ---------- | ------- | ----------------------------------------------- |
| 2025-11-08 | 1.1     | Unified contract (merged RendererDomainContract)|
| 2025-11-08 | 1.0     | Initial contract definition for Phase 2         |

---

_Contract enforcement begins: Phase 2 development cycle._

