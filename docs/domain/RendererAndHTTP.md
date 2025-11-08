# 🎨 Renderer and HTTP Telemetry Contract

**Version:** 1.0  
**Status:** Phase 3 - Active  
**Last Updated:** 2025-11-08

---

## 📋 Purpose

This contract defines the **rendering** and **HTTP telemetry** subsystems within the RetroVue Playout Engine. These components complete the media pipeline by consuming decoded frames and exposing operational metrics.

### Key Responsibilities

**FrameRenderer:**

- Consume frames from `FrameRingBuffer` in dedicated render thread
- Support multiple render modes: headless (production) and preview (debug)
- Track frame timing and render performance statistics
- Provide graceful degradation when buffer empty

**MetricsHTTPServer:**

- Serve Prometheus-compatible metrics over HTTP
- Provide non-blocking, thread-safe metrics access
- Support concurrent metric queries
- Expose channel state and performance data

---

## 🏗️ Architecture Overview

### Complete Pipeline Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│ Video File (MP4, MKV, AVI, etc.)                                │
└────────────────────────┬────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ FFmpegDecoder (Decode Thread)                                   │
│  ├─ libavformat: Container demuxing                             │
│  ├─ libavcodec: Video decoding (H.264, HEVC, etc.)             │
│  ├─ libswscale: Resolution scaling to 1920x1080                │
│  └─ Output: YUV420P frames with PTS/DTS metadata               │
└────────────────────────┬────────────────────────────────────────┘
                         │ Push
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ FrameRingBuffer (Lock-Free Circular Buffer)                     │
│  ├─ Capacity: 60 frames (~2 seconds @ 30fps)                   │
│  ├─ Atomic read/write indices                                   │
│  ├─ Single producer (decode thread)                             │
│  └─ Single consumer (render thread)                             │
└────────────────────────┬────────────────────────────────────────┘
                         │ Pop
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ FrameRenderer (Render Thread)                                   │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ Mode Selection (Factory Pattern)                          │ │
│  │  ├─ HeadlessRenderer: Production (no display)             │ │
│  │  └─ PreviewRenderer: Debug (SDL2 window)                  │ │
│  └───────────────────────────────────────────────────────────┘ │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │ Frame Consumption Loop                                     │ │
│  │  1. Pop frame from buffer (with timeout)                  │ │
│  │  2. RenderFrame() - virtual dispatch                      │ │
│  │  3. UpdateStats(render_time, frame_gap)                   │ │
│  │  4. Repeat until stopped                                  │ │
│  └───────────────────────────────────────────────────────────┘ │
└────────────────────────┬────────────────────────────────────────┘
                         │ Stats
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ MetricsExporter (Metrics Aggregation)                           │
│  ├─ Channel metrics (state, buffer depth, errors)              │
│  ├─ Thread-safe metric storage (std::mutex)                    │
│  └─ GenerateMetricsText() → Prometheus format                  │
└────────────────────────┬────────────────────────────────────────┘
                         │ Callback
                         ▼
┌─────────────────────────────────────────────────────────────────┐
│ MetricsHTTPServer (HTTP/1.1 Server Thread)                      │
│  ├─ Endpoint: GET /metrics (Prometheus text)                   │
│  ├─ Endpoint: GET / (info page)                                │
│  ├─ Non-blocking accept (100ms poll)                           │
│  ├─ Per-request timeout (5 seconds)                            │
│  └─ Port: 9308 (configurable)                                  │
└────────────────────────┬────────────────────────────────────────┘
                         │ HTTP
                         ▼
              [ Prometheus / Grafana / Curl ]
```

---

## 🎬 Renderer Interface

### Abstract Base Class

```cpp
namespace retrovue::renderer {

class FrameRenderer {
 public:
  virtual ~FrameRenderer();

  // Lifecycle
  bool Start();                           // Starts render thread
  void Stop();                            // Stops render thread gracefully
  bool IsRunning() const;                 // Thread state query

  // Statistics
  const RenderStats& GetStats() const;    // Performance metrics

  // Factory
  static std::unique_ptr<FrameRenderer> Create(
      const RenderConfig& config,
      buffer::FrameRingBuffer& input_buffer);

 protected:
  // Subclass hooks (called from render thread)
  virtual bool Initialize() = 0;          // One-time setup
  virtual void RenderFrame(const buffer::Frame& frame) = 0;
  virtual void Cleanup() = 0;             // Teardown

  void RenderLoop();                      // Main loop (runs in thread)
  void UpdateStats(double render_time_ms, double frame_gap_ms);

  RenderConfig config_;
  buffer::FrameRingBuffer& input_buffer_;
  RenderStats stats_;

  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::unique_ptr<std::thread> render_thread_;
};

}  // namespace retrovue::renderer
```

### Frame Timing Rules

| Rule                      | Specification                 | Rationale                        |
| ------------------------- | ----------------------------- | -------------------------------- |
| **Pop Timeout**           | 5ms sleep on empty buffer     | Prevents busy-wait CPU waste     |
| **Frame Gap Calculation** | `now - last_frame_time` in ms | Tracks actual render intervals   |
| **Stats Update**          | After every frame render      | Real-time performance monitoring |
| **Periodic Logging**      | Every 100 frames              | Prevents log spam                |
| **EMA Alpha**             | 0.1 for average render time   | Smooth out transient spikes      |

### Buffer Consumption Contract

```cpp
// Renderer MUST follow this pattern in RenderLoop():

while (!stop_requested_) {
    // 1. Attempt to pop frame
    buffer::Frame frame;
    if (!input_buffer_.Pop(frame)) {
        // Buffer empty - sleep and retry
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stats_.frames_skipped++;
        continue;  // DO NOT block indefinitely
    }

    // 2. Calculate frame gap
    auto now = std::chrono::steady_clock::now();
    double frame_gap_ms = duration_cast<milliseconds>(now - last_frame_time_);
    last_frame_time_ = now;

    // 3. Render frame (virtual dispatch)
    auto start = steady_clock::now();
    RenderFrame(frame);  // Subclass implementation
    auto end = steady_clock::now();

    // 4. Update statistics
    double render_time_ms = duration_cast<milliseconds>(end - start);
    UpdateStats(render_time_ms, frame_gap_ms);
}
```

**Guarantees:**

- MUST NOT block indefinitely on Pop()
- MUST update `frames_skipped` when buffer empty
- MUST calculate frame gap for every successful pop
- MUST call UpdateStats() after each render

---

## 🎭 Renderer Modes

### Mode Comparison

| Feature              | HeadlessRenderer     | PreviewRenderer                  |
| -------------------- | -------------------- | -------------------------------- |
| **Display Output**   | None                 | SDL2 window                      |
| **Use Case**         | Production playout   | Debug/development                |
| **Frame Processing** | Validation only      | YUV420 → SDL texture             |
| **Render Time**      | ~0.1ms               | ~2-5ms (1080p)                   |
| **Dependencies**     | None                 | SDL2 library                     |
| **Compilation Flag** | Always available     | `RETROVUE_SDL2_AVAILABLE`        |
| **Fallback**         | N/A                  | HeadlessRenderer if SDL2 missing |
| **Thread Safety**    | Single render thread | Single render thread             |
| **Memory**           | Minimal              | ~6MB (SDL textures)              |

### HeadlessRenderer Specification

**Purpose:** Validate pipeline operation without display hardware.

**Behavior:**

```cpp
bool HeadlessRenderer::Initialize() {
    std::cout << "[HeadlessRenderer] Initialized (no display output)" << std::endl;
    return true;  // Always succeeds
}

void HeadlessRenderer::RenderFrame(const buffer::Frame& frame) {
    // Headless mode: consume frame without rendering
    // Validates:
    //  - Frame data is present (frame.data.size() > 0)
    //  - Metadata is valid (pts, width, height)
    //  - Buffer consumption timing
    //
    // In production, this would push to:
    //  - SDI output hardware
    //  - Network streaming encoder
    //  - Recording pipeline
}

void HeadlessRenderer::Cleanup() {
    std::cout << "[HeadlessRenderer] Cleanup complete" << std::endl;
}
```

**Guarantees:**

- ✅ Never fails initialization
- ✅ Consumes frames at full speed
- ✅ No external dependencies
- ✅ Minimal CPU usage

### PreviewRenderer Specification

**Purpose:** Visual debugging of decoded frames.

**Behavior:**

```cpp
bool PreviewRenderer::Initialize() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << std::endl;
        return false;
    }

    window_ = SDL_CreateWindow(config_.window_title.c_str(),
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                config_.window_width,
                                config_.window_height,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    // ... create renderer and texture for YUV420P ...

    return window_ != nullptr && renderer_ != nullptr && texture_ != nullptr;
}

void PreviewRenderer::RenderFrame(const buffer::Frame& frame) {
    // Poll SDL events (window close, resize, etc.)
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            stop_requested_.store(true);
            return;
        }
    }

    // Update YUV texture with frame data
    SDL_UpdateYUVTexture(texture_, nullptr,
                         y_plane, y_pitch,
                         u_plane, uv_pitch,
                         v_plane, uv_pitch);

    // Render to window
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}

void PreviewRenderer::Cleanup() {
    SDL_DestroyTexture(texture_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}
```

**Conditional Compilation:**

```cpp
#ifdef RETROVUE_SDL2_AVAILABLE
    // Full SDL2 implementation
#else
    // Stub implementation that fails Initialize()
    bool PreviewRenderer::Initialize() {
        std::cerr << "ERROR: SDL2 not available" << std::endl;
        return false;
    }
#endif
```

**Guarantees:**

- ✅ Graceful fallback to HeadlessRenderer if SDL2 unavailable
- ✅ Window closes cleanly trigger stop
- ✅ YUV420P format support (no conversion needed)
- ✅ No tearing with VSYNC enabled

---

## 🌐 MetricsHTTPServer Contract

### Endpoints

#### GET /metrics

**Purpose:** Prometheus text exposition format

**Request:**

```http
GET /metrics HTTP/1.1
Host: localhost:9308
```

**Response (Success):**

```http
HTTP/1.1 200 OK
Content-Type: text/plain; version=0.0.4; charset=utf-8
Content-Length: 456
Connection: close

# HELP retrovue_playout_channel_state Channel state (0=stopped, 1=buffering, 2=ready, 3=error)
# TYPE retrovue_playout_channel_state gauge
retrovue_playout_channel_state{channel="1"} 2

# HELP retrovue_playout_buffer_depth_frames Current buffer depth in frames
# TYPE retrovue_playout_buffer_depth_frames gauge
retrovue_playout_buffer_depth_frames{channel="1"} 45

# HELP retrovue_playout_frame_gap_seconds Time since last frame in seconds
# TYPE retrovue_playout_frame_gap_seconds gauge
retrovue_playout_frame_gap_seconds{channel="1"} 0.033

# HELP retrovue_playout_decode_failure_count Total decode failures
# TYPE retrovue_playout_decode_failure_count counter
retrovue_playout_decode_failure_count{channel="1"} 0
```

**Guarantees:**

- MUST respond within 1 second
- MUST include `Content-Type: text/plain; version=0.0.4`
- MUST close connection after response
- MUST return empty body if no channels active

#### GET /

**Purpose:** Server info page

**Response:**

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 78
Connection: close

RetroVue Playout Engine - Metrics Server
Metrics available at: /metrics
```

#### Other Paths

**Response:**

```http
HTTP/1.1 404 Not Found
Content-Type: text/plain
Content-Length: 14
Connection: close

404 Not Found
```

### Concurrency Model

```
ServerLoop Thread:
    ↓
socket() → bind() → listen()
    ↓
while (!stop_requested_) {
    accept() [non-blocking, 100ms timeout]
        ↓
    if (client_socket valid) {
        HandleConnection(client_socket) [blocking on this thread]
            ↓
            recv() [5s timeout]
            ↓
            ParseRequest() → path
            ↓
            GenerateResponse() → metrics_callback_()
            ↓
            send() [blocking]
            ↓
        close(client_socket)
    }
}
```

**Threading Guarantees:**

- ✅ Single server thread per `MetricsHTTPServer` instance
- ✅ Sequential request handling (one at a time)
- ✅ Non-blocking accept with 100ms poll interval
- ✅ 5-second receive timeout per connection
- ✅ Metrics callback executed on server thread (thread-safe via mutex)

**Concurrency Limits:**
| Parameter | Value | Reason |
|-----------|-------|--------|
| Listen Backlog | 5 | Metrics queries are infrequent |
| Max Concurrent | 1 | Sequential is sufficient for metrics |
| Accept Timeout | 100ms | Allows clean shutdown |
| Recv Timeout | 5s | Prevents hung connections |
| Send Blocking | Yes | Metrics response small (<10KB) |

### Callback Pattern

```cpp
class MetricsHTTPServer {
 public:
  // Set callback before Start()
  void SetMetricsCallback(MetricsCallback callback);

  bool Start();  // Launches server thread
  void Stop();   // Signals stop and joins thread

 private:
  MetricsCallback metrics_callback_;  // std::function<std::string()>

  std::string GenerateResponse(const std::string& path) {
      if (path == "/metrics") {
          std::string metrics = metrics_callback_();  // Call user callback
          return FormatHTTPResponse(200, "text/plain; version=0.0.4", metrics);
      }
      // ... other paths ...
  }
};
```

**Usage:**

```cpp
auto http_server = std::make_unique<MetricsHTTPServer>(9308);

// Set callback (thread-safe lambda capture)
http_server->SetMetricsCallback([this]() {
    return this->GenerateMetricsText();  // MetricsExporter method
});

http_server->Start();  // Server thread begins accepting
```

**Callback Contract:**

- MUST be thread-safe (called from server thread)
- MUST return quickly (<10ms) to avoid blocking server
- MUST return valid Prometheus text or empty string
- SHOULD use mutex if accessing shared state

---

## 🔗 PlayoutService Integration

### Component Lifecycle

```cpp
// In StartChannel():

// 1. Create ring buffer
worker->ring_buffer = std::make_unique<FrameRingBuffer>(60);

// 2. Create and start producer (decode thread)
decode::ProducerConfig producer_config;
producer_config.asset_uri = plan_handle;
producer_config.stub_mode = false;  // Real decode
worker->producer = std::make_unique<FrameProducer>(producer_config, *worker->ring_buffer);
worker->producer->Start();

// 3. Create and start renderer (render thread)
renderer::RenderConfig render_config;
render_config.mode = renderer::RenderMode::HEADLESS;  // Production default
worker->renderer = renderer::FrameRenderer::Create(render_config, *worker->ring_buffer);
if (!worker->renderer->Start()) {
    std::cerr << "WARNING: Renderer failed, continuing without it" << std::endl;
    // Non-fatal: producer will fill buffer, just not consumed
}

// 4. Update metrics
UpdateChannelMetrics(channel_id);
```

**Integration Rules:**

| Rule                 | Specification                | Enforcement                                  |
| -------------------- | ---------------------------- | -------------------------------------------- |
| **Creation Order**   | Buffer → Producer → Renderer | Compile-time (constructor dependencies)      |
| **Start Order**      | Producer → Renderer          | Runtime (explicit sequencing)                |
| **Stop Order**       | Renderer → Producer          | Runtime (MUST stop consumer before producer) |
| **Renderer Failure** | Non-fatal warning            | Producer continues, buffer fills             |
| **Producer Failure** | Fatal error                  | StartChannel returns INTERNAL error          |
| **Mode Selection**   | Headless default             | Override via config for debug                |

### Update Plan Hot-Swap

```cpp
// In UpdatePlan():

// 1. Stop renderer first (consumer)
if (worker->renderer) {
    worker->renderer->Stop();
}

// 2. Stop producer (producer)
if (worker->producer) {
    worker->producer->Stop();
}

// 3. Clear buffer
worker->ring_buffer->Clear();

// 4. Recreate producer with new plan
producer_config.asset_uri = new_plan_handle;
worker->producer = std::make_unique<FrameProducer>(producer_config, *worker->ring_buffer);
worker->producer->Start();

// 5. Restart renderer
if (worker->renderer) {
    worker->renderer->Start();
}
```

**Hot-Swap Guarantees:**

- ✅ No deadlock (stop consumer before producer)
- ✅ Buffer cleared between plans
- ✅ Renderer survives plan changes
- ✅ Metrics reflect transition state

### Shutdown Sequence

```cpp
// In StopChannel() or destructor:

// 1. Stop renderer (consumer)
if (worker->renderer) {
    worker->renderer->Stop();  // Joins render thread
}

// 2. Stop producer (producer)
if (worker->producer) {
    worker->producer->Stop();  // Joins decode thread
}

// 3. Remove metrics
metrics_exporter_->RemoveChannel(channel_id);

// 4. RAII cleanup (buffer, worker destroyed)
```

**Shutdown Guarantees:**

- ✅ Threads joined before destruction
- ✅ No leaked threads
- ✅ Metrics cleaned up
- ✅ Timeout if thread won't join (implementation-defined)

---

## ✅ Testing Guarantees

### Render Rate Validation

**Test:** `test_renderer_frame_rate`

```cpp
TEST(FrameRendererTest, HeadlessRenderRate) {
    // Setup
    FrameRingBuffer buffer(60);
    RenderConfig config;
    config.mode = RenderMode::HEADLESS;

    auto renderer = FrameRenderer::Create(config, buffer);
    renderer->Start();

    // Fill buffer with 300 frames
    for (int i = 0; i < 300; ++i) {
        Frame frame = CreateTestFrame(i);
        ASSERT_TRUE(buffer.Push(frame));
    }

    // Wait for consumption
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Verify
    const auto& stats = renderer->GetStats();
    EXPECT_GT(stats.frames_rendered, 200);  // Should consume most frames
    EXPECT_LT(stats.average_render_time_ms, 1.0);  // Headless is fast

    renderer->Stop();
}
```

**Guarantees:**

- Headless renderer MUST process >200 fps
- Average render time MUST be <1ms
- Preview renderer MUST process >20 fps (with SDL2)

### Latency Validation

**Test:** `test_end_to_end_latency`

```cpp
TEST(PipelineTest, DecodeToRenderLatency) {
    // Setup complete pipeline
    FrameRingBuffer buffer(60);
    ProducerConfig prod_config;
    prod_config.stub_mode = true;  // Predictable timing

    auto producer = std::make_unique<FrameProducer>(prod_config, buffer);
    auto renderer = FrameRenderer::Create(RenderConfig{HEADLESS}, buffer);

    // Measure
    producer->Start();
    renderer->Start();

    auto start = steady_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto end = steady_clock::now();

    // Verify
    double elapsed_s = duration_cast<seconds>(end - start);
    uint64_t rendered = renderer->GetStats().frames_rendered;
    double fps = rendered / elapsed_s;

    EXPECT_GT(fps, 25.0);  // Should maintain ~30fps with headroom
    EXPECT_LT(fps, 35.0);

    producer->Stop();
    renderer->Stop();
}
```

**Guarantees:**

- End-to-end latency MUST be <100ms (buffer + processing)
- Frame rate MUST match producer rate ±20%
- No dropped frames under normal load

### HTTP Response Validation

**Test:** `test_http_metrics_endpoint`

```cpp
TEST(MetricsHTTPServerTest, MetricsEndpoint) {
    // Setup
    MetricsHTTPServer server(9308);
    bool callback_called = false;

    server.SetMetricsCallback([&callback_called]() {
        callback_called = true;
        return "# Test metric\ntest_value 42\n";
    });

    ASSERT_TRUE(server.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Query endpoint
    auto response = HttpGet("http://localhost:9308/metrics");

    // Verify
    EXPECT_EQ(response.status_code, 200);
    EXPECT_TRUE(response.headers.count("Content-Type"));
    EXPECT_EQ(response.headers["Content-Type"],
              "text/plain; version=0.0.4; charset=utf-8");
    EXPECT_TRUE(response.body.find("test_value 42") != std::string::npos);
    EXPECT_TRUE(callback_called);

    server.Stop();
}
```

**Guarantees:**

- MUST respond within 1 second
- MUST include correct Content-Type header
- MUST call metrics callback
- MUST close connection after response

### Concurrent Query Validation

**Test:** `test_concurrent_metrics_queries`

```cpp
TEST(MetricsHTTPServerTest, ConcurrentQueries) {
    MetricsHTTPServer server(9308);
    std::atomic<int> callback_count{0};

    server.SetMetricsCallback([&callback_count]() {
        callback_count++;
        return "test_metric 1\n";
    });

    server.Start();

    // Fire 10 concurrent queries
    std::vector<std::future<HttpResponse>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(std::async(std::launch::async, []() {
            return HttpGet("http://localhost:9308/metrics");
        }));
    }

    // Wait for all
    for (auto& f : futures) {
        auto response = f.get();
        EXPECT_EQ(response.status_code, 200);
    }

    EXPECT_EQ(callback_count, 10);  // All callbacks executed
    server.Stop();
}
```

**Guarantees:**

- MUST handle sequential concurrent queries
- MUST NOT corrupt responses
- MUST call callback once per query
- MUST NOT deadlock

---

## 🔐 Failure Modes

### Renderer Failures

| Failure                      | Detection                    | Recovery                            |
| ---------------------------- | ---------------------------- | ----------------------------------- |
| **SDL2 unavailable**         | `Initialize()` returns false | Fallback to HeadlessRenderer        |
| **Window closed by user**    | SDL_QUIT event               | `stop_requested_ = true`            |
| **Buffer perpetually empty** | `frames_skipped` > threshold | Log warning, continue               |
| **Render thread crash**      | Exception in `RenderLoop()`  | Thread exits, `IsRunning() = false` |

**Contract:** Renderer failure MUST NOT crash playout engine. Producer continues, buffer may fill.

### HTTP Server Failures

| Failure                 | Detection                     | Recovery                         |
| ----------------------- | ----------------------------- | -------------------------------- |
| **Port already in use** | `bind()` fails                | `Start()` returns false          |
| **Callback exception**  | Catch in `GenerateResponse()` | Return 500 Internal Server Error |
| **Client timeout**      | `recv()` timeout              | Close connection, continue       |
| **Malformed request**   | Parse error                   | Return 400 Bad Request           |

**Contract:** HTTP server failure MUST NOT affect playout operations. Metrics may be unavailable.

---

## 📊 Performance Requirements

### Renderer Performance

| Metric              | HeadlessRenderer | PreviewRenderer      |
| ------------------- | ---------------- | -------------------- |
| **Max Render Time** | 1ms              | 10ms                 |
| **Target FPS**      | 60+              | 30+                  |
| **CPU Usage**       | <1% per channel  | <5% per channel      |
| **Memory**          | <1MB             | <10MB (SDL textures) |
| **Startup Time**    | <10ms            | <500ms (SDL init)    |

### HTTP Server Performance

| Metric                 | Requirement                  |
| ---------------------- | ---------------------------- |
| **Request Latency**    | <1ms (avg), <10ms (p99)      |
| **Concurrent Queries** | 5 (listen backlog)           |
| **Throughput**         | 100+ req/s (single-threaded) |
| **Memory**             | <1KB per request             |
| **Startup Time**       | <100ms                       |

---

## 🔄 Version History

| Version | Date       | Changes                                                       |
| ------- | ---------- | ------------------------------------------------------------- |
| 1.0     | 2025-11-08 | Initial contract defining FrameRenderer and MetricsHTTPServer |

---

## 📚 Related Contracts

- [PlayoutEngine.md](PlayoutEngine.md) - Core channel lifecycle
- [MetricsAndTimingDomain.md](MetricsAndTimingDomain.md) - Metrics aggregation and timing contract
- [Phase3_Plan.md](../milestones/Phase3_Plan.md) - Implementation roadmap
- [Phase3_Part2_Complete.md](../milestones/Phase3_Part2_Complete.md) - Implementation details

---

**Contract Authority:** RetroVue Architecture Team  
**Implementation Status:** ✅ Complete (Phase 3 Part 2)  
**Test Coverage:** ✅ All integration tests passing
