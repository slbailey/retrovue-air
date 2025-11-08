# 🎬 Renderer Domain

_Related: [Playout Engine Domain](PlayoutEngineDomain.md) • [Renderer Contract](../contracts/RendererContract.md) • [Metrics and Timing Domain](MetricsAndTimingDomain.md) • [Phase 3 Plan](../milestones/Phase3_Plan.md)_

---

## 📋 Purpose & Role

The **Renderer** is the final stage in the RetroVue Playout Engine's media pipeline, responsible for consuming decoded video frames and preparing them for output. It acts as the bridge between the frame staging buffer and the ultimate destination—whether that's a display device, broadcast hardware, or validation pipeline.

### Pipeline Position

```
┌──────────────┐     ┌────────────────┐     ┌──────────────┐     ┌──────────────┐
│   Video      │     │   FFmpeg       │     │    Frame     │     │   Renderer   │
│   Asset      │────▶│   Decoder      │────▶│  RingBuffer  │────▶│  (Consumer)  │
│  (MP4/MKV)   │     │ (libavcodec)   │     │  (60 frames) │     │              │
└──────────────┘     └────────────────┘     └──────────────┘     └──────────────┘
      Source              Producer              Staging              Consumer
```

### Core Responsibilities

1. **Frame Consumption**: Pop frames from the `FrameRingBuffer` in real-time
2. **Output Delivery**: Render frames to the appropriate destination (display, hardware, validation)
3. **Timing Control**: Maintain frame pacing based on metadata timestamps
4. **Statistics Tracking**: Monitor render performance (FPS, gaps, skips)
5. **Graceful Degradation**: Handle empty buffers and transient errors without crashing

### Why Separate Renderer?

The renderer exists as a distinct component (rather than being part of the decoder or buffer) for several architectural reasons:

- **Separation of Concerns**: Decoding and rendering are independent operations with different performance characteristics
- **Thread Safety**: Dedicated render thread prevents blocking the decode pipeline
- **Flexibility**: Multiple renderer implementations (headless, preview, SDI output) without changing upstream code
- **Testing**: Headless mode enables full pipeline testing without display hardware
- **Scalability**: Different channels can use different renderer types based on requirements

---

## 🎭 Renderer Variants

The RetroVue Playout Engine provides two renderer implementations, selected at channel initialization based on operational requirements.

### HeadlessRenderer

**Purpose**: Production playout without visual output

**Use Cases**:

- Broadcasting to SDI/NDI hardware
- Network streaming (RTMP, SRT)
- Background recording pipelines
- Automated testing and CI/CD
- Headless server deployments

**Characteristics**:

```
┌─────────────────────────────────────┐
│       HeadlessRenderer              │
│                                     │
│  ┌───────────────────────────────┐ │
│  │ Pop frame from buffer         │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Validate metadata             │ │
│  │  • PTS present                │ │
│  │  • Data size correct          │ │
│  │  • Dimensions match           │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Update statistics             │ │
│  │  • frames_rendered++          │ │
│  │  • Calculate frame gap        │ │
│  │  • Update render FPS          │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Frame consumed                │ │
│  │ (no display output)           │ │
│  └───────────────────────────────┘ │
└─────────────────────────────────────┘

Performance: ~0.1ms per frame
Memory: <1MB
CPU: <1% per channel
Dependencies: None
```

**Key Features**:

- ✅ Zero external dependencies
- ✅ Ultra-low latency (<1ms per frame)
- ✅ Minimal CPU and memory footprint
- ✅ Always available (no compilation flags needed)
- ✅ Validates pipeline operation without display

### PreviewRenderer

**Purpose**: Visual debugging and development

**Use Cases**:

- Development and testing
- Operator preview monitors
- Quality assurance validation
- Frame-accurate inspection
- Demo and presentation scenarios

**Characteristics**:

```
┌─────────────────────────────────────┐
│       PreviewRenderer               │
│        (SDL2 Window)                │
│                                     │
│  ┌───────────────────────────────┐ │
│  │ Pop frame from buffer         │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Poll SDL events               │ │
│  │  • Window close               │ │
│  │  • Resize                     │ │
│  │  • Key press                  │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Update YUV texture            │ │
│  │  • Y plane (1920x1080)        │ │
│  │  • U plane (960x540)          │ │
│  │  • V plane (960x540)          │ │
│  └───────────────────────────────┘ │
│              ↓                      │
│  ┌───────────────────────────────┐ │
│  │ Render to window              │ │
│  │  • Clear                      │ │
│  │  • Copy texture               │ │
│  │  • Present (with VSYNC)       │ │
│  └───────────────────────────────┘ │
└─────────────────────────────────────┘

Performance: ~2-5ms per frame
Memory: ~10MB (SDL textures)
CPU: ~5% per channel
Dependencies: SDL2 library
```

**Key Features**:

- ✅ Real-time visual feedback
- ✅ Native YUV420 rendering (no conversion)
- ✅ VSYNC support for smooth playback
- ✅ Window controls (close, resize)
- ✅ Conditional compilation (falls back to headless if SDL2 unavailable)

### Variant Comparison

| Aspect           | HeadlessRenderer       | PreviewRenderer                    |
| ---------------- | ---------------------- | ---------------------------------- |
| **Output**       | None (validation only) | SDL2 window                        |
| **Latency**      | ~0.1ms                 | ~2-5ms                             |
| **CPU**          | <1%                    | ~5%                                |
| **Memory**       | <1MB                   | ~10MB                              |
| **Dependencies** | None                   | SDL2                               |
| **Use Case**     | Production             | Debug/QA                           |
| **Compilation**  | Always available       | Requires `RETROVUE_SDL2_AVAILABLE` |
| **Fallback**     | N/A                    | HeadlessRenderer if SDL2 missing   |

---

## ⚡ Design Principles

### 1. Real-Time Frame Pacing

The renderer operates in real-time, consuming frames as they become available without artificial delays (except for VSYNC in preview mode).

**Non-Blocking Consumption**:

```cpp
while (!stop_requested_) {
    Frame frame;
    if (!input_buffer_.Pop(frame)) {
        // Buffer empty - short sleep, don't block
        std::this_thread::sleep_for(5ms);
        stats_.frames_skipped++;
        continue;  // Never deadlock waiting for frames
    }

    RenderFrame(frame);  // Consume immediately
}
```

**Guarantees**:

- Renderer NEVER blocks indefinitely on buffer
- 5ms sleep prevents busy-wait CPU waste
- Tracks skipped frames when buffer empty
- Continues immediately when frames available

### 2. Low Latency

The renderer minimizes latency between frame availability and consumption to maintain real-time responsiveness.

**Latency Budget**:

```
Frame Available (buffer) → Pop → Render → Complete
     0ms                    <0.1ms  <5ms    <5.1ms total

Components:
  - Buffer pop:     <0.1ms  (lock-free atomic operation)
  - Frame render:   0.1-5ms (headless: 0.1ms, preview: 2-5ms)
  - Stats update:   <0.1ms  (atomic counters, EMA calculation)
```

**Optimization Techniques**:

- Lock-free buffer operations (atomic indices)
- Single render thread per channel (no context switching)
- Zero-copy frame data (reference semantics)
- Minimal statistics overhead (EMA instead of full history)

### 3. Graceful Degradation

The renderer handles adverse conditions without crashing or blocking the pipeline.

**Failure Modes & Responses**:

| Condition            | Detection               | Response                               | Impact               |
| -------------------- | ----------------------- | -------------------------------------- | -------------------- |
| **Buffer Empty**     | Pop returns false       | Sleep 5ms, increment `frames_skipped`  | Temporary output gap |
| **SDL2 Unavailable** | Initialize fails        | Fallback to HeadlessRenderer           | No visual output     |
| **Window Closed**    | SDL_QUIT event          | Set `stop_requested_`, exit gracefully | Renderer stops       |
| **Slow Render**      | Render time > threshold | Log warning, continue                  | Possible frame drops |

**Non-Fatal Renderer**:

```cpp
// In PlayoutService::StartChannel():
if (!worker->renderer->Start()) {
    std::cerr << "WARNING: Renderer failed, continuing without it" << std::endl;
    // Producer continues filling buffer
    // Pipeline operates, just no output consumption
}
```

The renderer is intentionally **non-critical**: decode and buffering continue even if rendering fails, ensuring maximum uptime.

---

## 🔗 Integration Points

### FrameRingBuffer

**Relationship**: Consumer

The renderer is the **sole consumer** of frames from the `FrameRingBuffer`.

```
FrameRingBuffer                    FrameRenderer
┌─────────────────┐               ┌──────────────────┐
│ write_index (W) │               │  RenderLoop()    │
│                 │               │                  │
│  [Frame][Frame] │               │  while running:  │
│  [Frame][Frame] │  ──Pop()────▶ │    frame = Pop() │
│  [Frame][Frame] │               │    RenderFrame() │
│                 │               │    UpdateStats() │
│ read_index (R)  │               │                  │
└─────────────────┘               └──────────────────┘
   Producer: Decode                 Consumer: Render
```

**Contract**:

- Renderer calls `buffer.Pop(frame)` in tight loop
- Returns false when buffer empty (non-blocking)
- Never modifies buffer state except read index
- Single consumer guarantee (one render thread per buffer)

### MetricsExporter

**Relationship**: Statistics Source

The renderer reports performance metrics to the `MetricsExporter` for telemetry.

```cpp
// Renderer → Metrics flow
void FrameRenderer::UpdateStats(double render_time_ms, double frame_gap_ms) {
    stats_.frames_rendered++;
    stats_.frame_gap_ms = frame_gap_ms;
    stats_.average_render_time_ms = EMA(render_time_ms);
    stats_.current_render_fps = 1000.0 / frame_gap_ms;
}

// PlayoutService reads these stats:
const auto& render_stats = worker->renderer->GetStats();

// Updates channel metrics:
channel_metrics.buffer_depth_frames = worker->ring_buffer->Size();
channel_metrics.frame_gap_seconds = render_stats.frame_gap_ms / 1000.0;
```

**Metrics Provided**:

- `frames_rendered`: Total frames consumed
- `frames_skipped`: Buffer empty events
- `average_render_time_ms`: Rendering latency
- `current_render_fps`: Real-time render rate
- `frame_gap_ms`: Time between frames

### PlayoutService

**Relationship**: Lifecycle Manager

The `PlayoutService` creates, starts, updates, and stops renderers as part of channel lifecycle management.

```cpp
// Channel Lifecycle with Renderer
StartChannel(plan_handle):
    1. Create FrameRingBuffer
    2. Create & start FrameProducer (decode thread)
    3. Create & start FrameRenderer (render thread)  ← Renderer enters here
    4. Update metrics

UpdatePlan(new_plan_handle):
    1. Stop FrameRenderer                           ← Stop consumer first
    2. Stop FrameProducer
    3. Clear buffer
    4. Restart FrameProducer with new plan
    5. Restart FrameRenderer                        ← Restart consumer

StopChannel():
    1. Stop FrameRenderer                           ← Stop consumer first
    2. Stop FrameProducer
    3. Remove metrics
```

**Integration Contract**:

- Renderer created AFTER buffer and producer
- Renderer stopped BEFORE producer (consumer before producer)
- Renderer failure non-fatal (warning logged, producer continues)
- Renderer lifecycle independent of decode errors

---

## 🧵 Thread Model

### Dedicated Render Thread

Each channel's renderer runs in its own dedicated thread, independent of the decode thread and main gRPC service thread.

```
┌────────────────────────────────────────────────────────────────┐
│                       Process Space                            │
│                                                                │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐       │
│  │ gRPC Thread  │  │ Decode Thread│  │ Render Thread│       │
│  │              │  │              │  │              │       │
│  │ StartChannel │  │ while (1) {  │  │ while (1) {  │       │
│  │ UpdatePlan   │  │   Decode()   │  │   Pop()      │       │
│  │ StopChannel  │  │   Push()     │  │   Render()   │       │
│  │ GetVersion   │  │ }            │  │   Stats()    │       │
│  │              │  │              │  │ }            │       │
│  └──────────────┘  └──────────────┘  └──────────────┘       │
│         │                  │                  │               │
│         └──────────────────┴──────────────────┘               │
│                           │                                    │
│                    Shared Resources                            │
│                  (protected by atomics)                        │
│                                                                │
│         ┌─────────────────────────────────────┐               │
│         │      FrameRingBuffer                │               │
│         │  • write_index (atomic)             │               │
│         │  • read_index (atomic)              │               │
│         │  • buffer (fixed array)             │               │
│         └─────────────────────────────────────┘               │
└────────────────────────────────────────────────────────────────┘
```

### Thread Characteristics

| Aspect           | Specification                                    |
| ---------------- | ------------------------------------------------ |
| **Creation**     | `std::thread` in `FrameRenderer::Start()`        |
| **Entry Point**  | `FrameRenderer::RenderLoop()` (protected method) |
| **Lifetime**     | Created on Start(), joined on Stop()             |
| **Priority**     | Normal (OS default)                              |
| **CPU Affinity** | None (OS scheduler decides)                      |
| **Stack Size**   | OS default (~1MB on most platforms)              |

### Synchronization Logic

The renderer uses minimal synchronization to maximize performance:

**Atomic Operations**:

```cpp
// Buffer read/write coordination (lock-free)
std::atomic<uint32_t> FrameRingBuffer::read_index_;
std::atomic<uint32_t> FrameRingBuffer::write_index_;

// Renderer state flags
std::atomic<bool> FrameRenderer::running_;
std::atomic<bool> FrameRenderer::stop_requested_;

// Statistics counters
std::atomic<uint64_t> FrameRenderer::frames_rendered_;
std::atomic<uint64_t> FrameRenderer::frames_skipped_;
```

**No Mutexes in Hot Path**:

- Buffer Pop() uses only atomic compare-and-swap
- Statistics use atomic increments
- No condition variables or locks in render loop
- Thread join only at shutdown (not in steady-state)

**Memory Ordering**:

```cpp
// Stop signal (sequentially consistent)
stop_requested_.store(true, std::memory_order_release);
// ... on render thread ...
if (stop_requested_.load(std::memory_order_acquire)) break;

// Statistics (relaxed for performance)
frames_rendered_.fetch_add(1, std::memory_order_relaxed);
```

### Timing Source

The renderer derives timing from **frame metadata** (PTS) rather than wall-clock time.

```cpp
// Frame timing from metadata
void FrameRenderer::RenderLoop() {
    auto last_frame_time = steady_clock::now();

    while (!stop_requested_) {
        Frame frame;
        if (buffer.Pop(frame)) {
            auto now = steady_clock::now();
            double frame_gap_ms = duration<ms>(now - last_frame_time);
            last_frame_time = now;

            // Render frame (timing controlled by buffer availability)
            RenderFrame(frame);

            // Stats based on actual intervals, not PTS
            UpdateStats(render_time_ms, frame_gap_ms);
        }
    }
}
```

**Timing Philosophy**:

- **Source of Truth**: Decoder PTS (from source media)
- **Pacing**: Buffer availability (back-pressure from renderer)
- **Measurement**: Wall-clock intervals (for stats)
- **No Sleep**: Renderer never artificially delays (except 5ms on empty buffer)

This design allows the renderer to adapt to varying decode rates and buffer conditions while maintaining accurate performance measurement.

---

## 🚀 Future Extensions

The current renderer implementation provides a solid foundation for advanced features planned for future phases.

### GPU Acceleration

**Goal**: Offload rendering to GPU for improved performance and capability.

**Potential Technologies**:

- **Vulkan**: Modern, cross-platform, explicit control
- **OpenGL**: Mature, widely supported, easier integration
- **DirectX 12** (Windows): Native Windows acceleration
- **Metal** (macOS): Native Apple silicon support

**Benefits**:

```
Current: CPU Render (~2-5ms per frame, 1080p)
    ↓
With GPU: GPU Render (~0.5-1ms per frame, 4K possible)
    ↓
Enables:
  • 4K/8K rendering
  • Multiple simultaneous channels
  • Real-time effects
  • Lower CPU utilization
```

**Architecture Sketch**:

```cpp
class GPURenderer : public FrameRenderer {
 protected:
    bool Initialize() override {
        // Initialize Vulkan/OpenGL context
        // Create textures for YUV planes
        // Set up render pipeline
    }

    void RenderFrame(const Frame& frame) override {
        // Upload frame to GPU texture
        // Execute shader pipeline
        // Present to window or framebuffer
    }
};
```

### Shader-Based Compositing

**Goal**: Real-time graphics overlay and compositing.

**Use Cases**:

- Station logos and bugs
- Lower-thirds and tickers
- Transition effects
- Multi-source compositing
- Real-time color grading

**Example Pipeline**:

```
Frame (YUV420)
    ↓
GPU Upload
    ↓
YUV → RGB Shader
    ↓
Composition Shader
  • Video layer (base)
  • Graphics layer (overlay)
  • Text layer (titles)
    ↓
RGB → YUV Shader (if needed)
    ↓
Output (Display or Encoder)
```

### Multi-Output Rendering

**Goal**: Single decode, multiple output destinations.

**Architecture**:

```
                ┌─────────────────┐
                │  FrameProducer  │
                │   (Decode 1x)   │
                └────────┬────────┘
                         │
                    Single Decode
                         │
                ┌────────▼─────────┐
                │  FrameRingBuffer │
                └────────┬─────────┘
                         │
            ┌────────────┼────────────┐
            │            │            │
     ┌──────▼─────┐ ┌───▼──────┐ ┌──▼───────┐
     │ SDI Output │ │ Preview  │ │ Network  │
     │  Renderer  │ │ Renderer │ │ Renderer │
     └────────────┘ └──────────┘ └──────────┘
```

**Benefits**:

- One decode feeds multiple outputs
- Reduced CPU/bandwidth usage
- Synchronized output streams
- Independent failure domains

### Hardware-Accelerated Output

**Goal**: Direct integration with broadcast hardware.

**Potential Targets**:

- **SDI Cards**: Blackmagic DeckLink, AJA Kona
- **NDI**: Network Device Interface (NewTek)
- **RTMP/SRT**: Direct network streaming
- **V4L2**: Linux video output devices

**Example Integration**:

```cpp
class SDIRenderer : public FrameRenderer {
 protected:
    void RenderFrame(const Frame& frame) override {
        // Convert YUV420 to SDI format (4:2:2, 10-bit)
        // Write to DeckLink card via SDK
        // Handle output timing (genlock)
    }
};
```

### Frame-Accurate Playout

**Goal**: Precise frame timing for broadcast compliance.

**Requirements**:

- Genlock synchronization
- Black burst or tri-level sync
- Sub-frame accurate output
- Jitter compensation

**Technique**:

```cpp
void FrameRenderer::RenderLoop() {
    // Wait for genlock pulse
    WaitForGenlockPulse();

    // Pop frame exactly on boundary
    Frame frame;
    buffer.Pop(frame);

    // Render with zero jitter
    RenderFrameImmediate(frame);
}
```

---

## 📚 Related Documentation

- **[Renderer Contract](../contracts/RendererContract.md)** — Detailed API contract and specifications
- **[Playout Engine Domain](PlayoutEngineDomain.md)** — Overall playout engine domain model
- **[Metrics and Timing Domain](MetricsAndTimingDomain.md)** — Time synchronization and telemetry
- **[Phase 3 Complete](../milestones/Phase3_Complete.md)** — Implementation completion summary
- **[README](../../README.md)** — Quick start and usage guide

---

## 🎯 Summary

The **Renderer Domain** completes the RetroVue Playout Engine's media pipeline, providing flexible, high-performance frame consumption with multiple output modes:

**Key Capabilities**:

- ✅ Dual renderer modes (headless production + preview debug)
- ✅ Real-time frame pacing with low latency
- ✅ Lock-free buffer integration
- ✅ Comprehensive performance statistics
- ✅ Graceful degradation under adverse conditions
- ✅ Non-critical operation (pipeline continues on renderer failure)

**Design Philosophy**:

- **Simplicity**: Minimal synchronization, clear responsibilities
- **Performance**: Lock-free, low-latency, efficient
- **Reliability**: Non-blocking, graceful errors, fail-safe
- **Extensibility**: Abstract interface, factory pattern, future-ready

**Future-Ready**:
The current architecture provides a solid foundation for advanced features including GPU acceleration, shader compositing, and hardware integration while maintaining backward compatibility and operational stability.

---

_Last Updated: 2025-11-08 | Phase 3 Part 2 Complete_
