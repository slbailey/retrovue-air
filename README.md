# RetroVue Playout Engine

**Native C++ playout engine for broadcasting video content with frame-accurate timing.**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C++-20-blue.svg)](https://isocpp.org/)
[![CMake](https://img.shields.io/badge/CMake-3.15+-blue.svg)](https://cmake.org/)

---

## 🎯 Project Status

| Phase       | Status         | Description                                     |
| ----------- | -------------- | ----------------------------------------------- |
| **Phase 1** | ✅ Complete    | gRPC skeleton + proto definitions               |
| **Phase 2** | ✅ Complete    | Frame buffer + stub decode + metrics            |
| **Phase 3** | 🚧 In Progress | **FFmpeg decoder ✅** + Renderer + HTTP metrics |
| Phase 4     | 📋 Planned     | Production hardening + multi-channel            |

### Latest Milestone: FFmpeg Decoder Implementation

- ✅ Real video decoding with libavformat/libavcodec
- ✅ Multi-codec support (H.264, HEVC, etc.)
- ✅ Resolution scaling and YUV420P output
- ✅ Conditional compilation (works without FFmpeg)
- ✅ Performance monitoring and error handling
- ✅ All tests passing

**Next:** Renderer integration and HTTP metrics server

---

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────┐
│ Python ChannelManager (Retrovue Core)                  │
│  └─ gRPC Client                                         │
└───────────────────────┬─────────────────────────────────┘
                        │ gRPC (proto/retrovue/playout.proto)
┌───────────────────────▼─────────────────────────────────┐
│ C++ Playout Engine                                      │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ PlayoutControlImpl (gRPC Service)                   │ │
│ │  ├─ StartChannel(plan_handle)                       │ │
│ │  ├─ UpdatePlan(plan_handle)                         │ │
│ │  └─ StopChannel()                                   │ │
│ └─────────────────────────────────────────────────────┘ │
│                         ↓                                │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ FrameProducer (decode thread)                       │ │
│ │  ├─ FFmpegDecoder (libav*)                          │ │
│ │  │   ├─ Format detection                            │ │
│ │  │   ├─ Video decoding                              │ │
│ │  │   └─ Resolution scaling                          │ │
│ │  └─ Push to FrameRingBuffer                         │ │
│ └─────────────────────────────────────────────────────┘ │
│                         ↓                                │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ FrameRingBuffer (lock-free circular buffer)         │ │
│ │  └─ 60 frames @ 1920x1080 YUV420P                  │ │
│ └─────────────────────────────────────────────────────┘ │
│                         ↓                                │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ Renderer (Phase 3 - in progress)                    │ │
│ │  ├─ Preview window (debug)                          │ │
│ │  └─ Headless mode (production)                      │ │
│ └─────────────────────────────────────────────────────┘ │
│                         ↓                                │
│ ┌─────────────────────────────────────────────────────┐ │
│ │ MetricsExporter (Prometheus)                        │ │
│ │  └─ HTTP server @ localhost:9308/metrics            │ │
│ └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

---

## 🚀 Quick Start

### Prerequisites

**Required:**

- CMake 3.15+
- C++20 compiler (MSVC 2019+, GCC 10+, Clang 11+)
- vcpkg (for dependencies)
- gRPC + Protobuf (via vcpkg)

**Optional (for real video decoding):**

- FFmpeg development libraries (libavformat, libavcodec, libavutil, libswscale)

### Build

```powershell
# Clone repository
git clone https://github.com/your-org/Retrovue-playout.git
cd Retrovue-playout

# Configure (assumes vcpkg installed)
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

# Build
cmake --build build --config Debug
```

### Run

```powershell
# Start playout engine
.\build\Debug\retrovue_playout.exe --port 50051

# In another terminal, test with Python client
python scripts\test_server.py
```

**Expected Output:**

```
[TEST 1] GetVersion              [PASS]
[TEST 2] StartChannel            [PASS]
[TEST 3] UpdatePlan              [PASS]
[TEST 4] StopChannel             [PASS]

[SUCCESS] All tests passed!
```

---

## 📦 Components

### Core Modules

| Module             | Path                         | Description                           |
| ------------------ | ---------------------------- | ------------------------------------- |
| **gRPC Service**   | `src/playout_service.*`      | PlayoutControl API implementation     |
| **Frame Buffer**   | `src/buffer/`                | Lock-free circular buffer (60 frames) |
| **Frame Producer** | `src/decode/FrameProducer.*` | Decode thread orchestrator            |
| **FFmpeg Decoder** | `src/decode/FFmpegDecoder.*` | Real video decoding (Phase 3)         |
| **Metrics**        | `src/telemetry/`             | Prometheus metrics exporter           |
| **Proto**          | `proto/retrovue/`            | gRPC service definitions              |

### Header Structure

Following [development standards](docs/development-standards.md):

```
include/retrovue/
├─ buffer/
│  └─ FrameRingBuffer.h
├─ decode/
│  ├─ FrameProducer.h
│  └─ FFmpegDecoder.h
└─ telemetry/
   └─ MetricsExporter.h
```

---

## 🧪 Testing

### Unit Tests

```powershell
# Run buffer tests (requires GTest)
.\build\Debug\test_buffer.exe

# Run decode tests
.\build\Debug\test_decode.exe
```

### Integration Tests

```powershell
# Start server
.\build\Debug\retrovue_playout.exe

# Run Python test suite
python scripts\test_server.py
```

---

## 📚 Documentation

**[→ Complete Documentation Index](docs/README.md)**

### Quick Reference

| Document                                                           | Description                    |
| ------------------------------------------------------------------ | ------------------------------ |
| [Architecture Overview](docs/architecture/ArchitectureOverview.md) | System architecture and design |
| [Playout Engine Domain](docs/domain/PlayoutEngineDomain.md)        | Core domain model              |
| [Playout Engine Contract](docs/contracts/PlayoutEngineContract.md) | gRPC API specification         |
| [Quick Start](docs/developer/QuickStart.md)                        | Getting started guide          |
| [Build & Debug](docs/developer/BuildAndDebug.md)                   | Development workflow           |
| [Development Standards](docs/developer/DevelopmentStandards.md)    | Code structure guidelines      |

### Development Roadmap

**[→ Full Roadmap](docs/milestones/Roadmap.md)**

- ✅ Phase 1: gRPC Skeleton
- ✅ Phase 2: Frame Buffer & Stub Decode
- ✅ Phase 3: FFmpeg Decoder + Renderer + HTTP Metrics
- 📋 Phase 4: Production Hardening (Planned)

---

## 🔧 Configuration

### FFmpeg Integration

The playout engine supports both **stub mode** (synthetic frames) and **real decode mode** (FFmpeg).

#### Without FFmpeg (Stub Mode)

Build proceeds normally without FFmpeg:

```powershell
cmake -S . -B build
cmake --build build
```

**Behavior:**

- Generates synthetic test frames
- No external dependencies required
- Good for development and testing

#### With FFmpeg (Real Decode)

Install FFmpeg and rebuild:

**Windows (vcpkg):**

```powershell
vcpkg install ffmpeg:x64-windows
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

**Linux:**

```bash
sudo apt install libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
cmake -S . -B build
cmake --build build
```

**Behavior:**

- Decodes real video files (MP4, MKV, etc.)
- Supports H.264, HEVC, and other codecs
- Production-ready performance

---

## 🎯 Roadmap

### Phase 3 (Current)

- ✅ FFmpegDecoder implementation
- 🚧 FrameRenderer (preview + headless)
- 🚧 HTTP metrics server
- 🚧 MasterClock integration

### Phase 4 (Next)

- Multi-channel support
- Hardware acceleration (NVDEC, QSV)
- Frame-accurate timing
- Production hardening

---

## 🤝 Contributing

1. Follow [development standards](docs/development-standards.md)
2. Run tests before committing
3. Update documentation with changes
4. Use conventional commit messages

---

## 📄 License

MIT License - see [LICENSE](LICENSE) for details

---

## 🔗 Related Projects

- [Retrovue Core](https://github.com/your-org/Retrovue) - Python media asset manager
- [Retrovue Web](https://github.com/your-org/Retrovue-web) - Web UI

---

## 📞 Support

- Issues: [GitHub Issues](https://github.com/your-org/Retrovue-playout/issues)
- Docs: [docs/](docs/)
- Contact: dev@retrovue.io

---

**Built with ❤️ by the RetroVue Team**
