# RetroVue Playout Engine – Roadmap

_Related: [Architecture Overview](../architecture/ArchitectureOverview.md) • [PROJECT_OVERVIEW](../PROJECT_OVERVIEW.md)_

---

## Overview

This document tracks the development roadmap for the RetroVue Playout Engine, a native C++ subsystem responsible for frame-accurate video playout. The roadmap is organized into phases, each delivering specific capabilities toward a production-ready broadcast playout system.

---

## Phase Status

| Phase       | Status         | Description                                     | Plan | Completion |
| ----------- | -------------- | ----------------------------------------------- | ---- | ---------- |
| **Phase 1** | ✅ Complete    | gRPC skeleton + proto definitions               | N/A  | [Phase1_Complete.md](Phase1_Complete.md) |
| **Phase 2** | ✅ Complete    | Frame buffer + stub decode + metrics            | [Phase2_Plan.md](Phase2_Plan.md) | [Phase2_Complete.md](Phase2_Complete.md) |
| **Phase 3** | ✅ Complete    | FFmpeg decoder + Renderer + HTTP metrics        | [Phase3_Plan.md](Phase3_Plan.md) | [Phase3_Complete.md](Phase3_Complete.md) |
| **Phase 4** | 📋 Planned     | Production hardening + multi-channel            | TBD | TBD |

---

## Milestone Documentation Structure

Each development phase follows a standardized documentation pattern:

- **`PhaseN_Plan.md`** - Goals, objectives, and planned deliverables
- **`PhaseN_Complete.md`** - Implementation summary and results

Detailed implementation records are archived in the [`archive/`](archive/) directory for reference.

---

## Phase 1: Skeleton

**Status:** ✅ Complete  
**Completed:** Initial development milestone  
**Milestone Document:** [Phase1_Skeleton.md](Phase1_Skeleton.md)

### Goals

Establish the foundational gRPC service interface and build infrastructure.

### Deliverables

- ✅ gRPC service definition (`proto/retrovue/playout.proto`)
- ✅ PlayoutControlImpl service implementation
- ✅ Basic RPC methods: `StartChannel`, `UpdatePlan`, `StopChannel`, `GetVersion`
- ✅ CMake build system
- ✅ Python test client
- ✅ gRPC health check and reflection

### Key Components

- **PlayoutControl gRPC Service**: Control plane API for channel lifecycle management
- **CMake Build System**: Cross-platform build with vcpkg integration
- **Python Test Suite**: Integration tests via `scripts/test_server.py`

### Validation

- All RPC methods implemented and tested
- gRPC server starts successfully
- Python client can interact with service
- Clean error handling and logging

---

## Phase 2: Decode & Frame Bus

**Status:** ✅ Complete  
**Completed:** Initial development milestone  
**Milestone Documents:** [Phase2_Complete.md](Phase2_Complete.md) • [Refactoring_Complete.md](Refactoring_Complete.md)

### Goals

Build the frame pipeline infrastructure with lock-free buffer and stub frame generation.

### Deliverables

- ✅ Lock-free circular frame buffer (`FrameRingBuffer`)
- ✅ Frame producer with stub decode (synthetic frames)
- ✅ Dedicated decode thread per channel
- ✅ Prometheus metrics schema
- ✅ Console metrics logging (stub mode)
- ✅ Unit tests for buffer and producer
- ✅ Code structure refactoring (standards compliance)

### Key Components

- **FrameRingBuffer**: Lock-free circular buffer (60 frames @ 1920x1080 YUV420P)
- **FrameProducer**: Decode thread orchestrator with stub frame generation
- **MetricsExporter**: Prometheus-format metrics (console logging)
- **Standards-Compliant Structure**: Public API headers in `include/retrovue/`

### Validation

- Frame producer generates stable 30fps stream
- Ring buffer handles producer/consumer patterns
- No buffer overflows or underruns
- All integration tests passing
- Code structure follows RetroVue standards

---

## Phase 3: Real Decode + Renderer

**Status:** ✅ Complete  
**Completed:** Current milestone  
**Milestone Documents:** [Phase3_Plan.md](Phase3_Plan.md) • [Phase3_FFmpeg.md](Phase3_FFmpeg.md) • [Phase3_Part2_Complete.md](Phase3_Part2_Complete.md)

### Goals

Implement real video decoding, frame rendering, and HTTP metrics server.

### Part 1: FFmpeg Decoder ✅

- ✅ Real video decoding with libavformat/libavcodec
- ✅ Multi-codec support (H.264, HEVC, etc.)
- ✅ Resolution scaling and YUV420P output
- ✅ Conditional compilation (works without FFmpeg)
- ✅ Performance monitoring and error handling
- ✅ Graceful fallback to stub mode

### Part 2: Renderer & HTTP Metrics ✅

- ✅ FrameRenderer interface (headless + preview modes)
- ✅ SDL2-based preview window (optional)
- ✅ Headless renderer for production
- ✅ MetricsHTTPServer with real HTTP/1.1 implementation
- ✅ Prometheus metrics endpoint at `/metrics`
- ✅ Complete decode → render → metrics pipeline

### Key Components

- **FFmpegDecoder**: Real video decoding using FFmpeg libraries
- **FrameRenderer**: Abstract base class with headless/preview implementations
- **MetricsHTTPServer**: Native socket-based HTTP server for metrics
- **Complete Pipeline**: Video file → Decode → Buffer → Render → Metrics

### Validation

- Real video files decode successfully
- HTTP metrics endpoint responds correctly
- Headless mode validates frame consumption
- Preview mode displays frames (when SDL2 available)
- All integration tests passing

---

## Phase 4: Production Readiness

**Status:** 📋 Planned  
**Target:** Future development

### Goals

Harden the system for 24/7 production broadcast operations.

### Planned Deliverables

#### 4.1 Master Clock Integration

- [ ] Implement MasterClock synchronization
- [ ] Frame-accurate PTS alignment
- [ ] Timing drift detection and correction
- [ ] Frame gap calculation and reporting
- [ ] Clock source abstraction (system time, NTP, PTP)

#### 4.2 Multi-Channel Support

- [ ] Support multiple concurrent channels
- [ ] Per-channel resource isolation
- [ ] Independent channel lifecycle management
- [ ] Multi-channel metrics aggregation
- [ ] Resource limits and quotas

#### 4.3 Hardware Acceleration

- [ ] NVDEC support (NVIDIA GPUs)
- [ ] Intel Quick Sync Video (QSV)
- [ ] VideoToolbox (macOS)
- [ ] VAAPI (Linux)
- [ ] Automatic fallback to software decode

#### 4.4 Error Recovery & Resilience

- [ ] Automatic retry with exponential backoff
- [ ] Slate frame fallback on decode failure
- [ ] Watchdog for decode thread health
- [ ] Graceful degradation modes
- [ ] Comprehensive error logging

#### 4.5 Performance Optimization

- [ ] Zero-copy frame transfer
- [ ] NUMA-aware thread pinning
- [ ] Lock-free metrics updates
- [ ] Memory pool for frame buffers
- [ ] CPU affinity configuration

#### 4.6 Operational Features

- [ ] Hot reload of configuration
- [ ] Dynamic metrics scraping interval
- [ ] Health check endpoints
- [ ] Prometheus alerting rules
- [ ] Grafana dashboard templates
- [ ] Production deployment guide

---

## Future Enhancements

### Beyond Phase 4

#### Advanced Rendering

- Hardware-accelerated output (OpenGL, Vulkan)
- Multiple output targets (SDI, NDI, RTMP)
- On-screen graphics overlay
- Real-time video effects

#### Advanced Telemetry

- Distributed tracing (OpenTelemetry)
- Advanced performance profiling
- Frame-level timing analytics
- Anomaly detection

#### Integration

- Python bindings (pybind11)
- REST API for control plane
- WebSocket event streaming
- Integration with RetroVue scheduling system

---

## Development Principles

### Contract-First Development

All features must:
1. Define the contract/interface first
2. Implement tests before implementation
3. Document expected behavior
4. Validate against contract

### Standards Compliance

Code must follow:
- [Development Standards](../development-standards.md)
- Public API headers in `include/retrovue/`
- Proper namespace hierarchy
- Modern C++20 practices

### Testing Strategy

- **Unit Tests**: Per-component validation (GTest)
- **Integration Tests**: End-to-end pipeline tests
- **Performance Tests**: Latency and throughput benchmarks
- **Stress Tests**: Long-running stability validation

### Documentation Requirements

Each phase must include:
- High-level architecture updates
- Domain model documentation
- Contract specifications
- Operator guides
- Developer guides

---

## Historical Milestones

### Detailed Completion Records

- [Phase 1 Complete](Phase1_Complete.md) – Initial gRPC service implementation
- [Phase 2 Plan](Phase2_Plan.md) – Phase 2 planning and objectives
- [Phase 2 Complete](Phase2_Complete.md) – Frame buffer and stub decode
- [Phase 3 Plan](Phase3_Plan.md) – Phase 3 overview and goals
- [Phase 3 Complete](Phase3_Complete.md) – FFmpeg decoder, renderer, and HTTP metrics

**Detailed Implementation Records:**
- [Phase 3 FFmpeg Details](archive/Phase3_FFmpeg_Details.md) – Deep dive into decoder implementation
- [Phase 3 Renderer Details](archive/Phase3_Renderer_Details.md) – Deep dive into renderer and metrics
- [Refactoring Complete](archive/Refactoring_Complete.md) – Standards compliance refactoring

---

## Related Documentation

- [Architecture Overview](../architecture/ArchitectureOverview.md)
- [Playout Engine Domain](../domain/PlayoutEngineDomain.md)
- [Playout Engine Contract](../contracts/PlayoutEngineContract.md)
- [Development Standards](../developer/DevelopmentStandards.md)
- [Quick Start](../developer/QuickStart.md)
- [Build & Debug](../developer/BuildAndDebug.md)

---

**Last Updated:** 2025-11-08  
**Status:** Phase 3 Complete ✅

