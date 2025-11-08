# RetroVue Playout Engine – Developer Overview

_Related: [Architecture overview](architecture/ArchitectureOverview.md) • [Runtime model](runtime/PlayoutRuntime.md) • [Proto schema](../proto/retrovue/playout.proto)_

---

## Purpose

The **RetroVue Playout Engine** implements a native C++ backend that transforms playout plans (from the ChannelManager) into real-time decoded frame streams. It serves as the bridge between high-level Python orchestration and the low-level frame delivery required by the Renderer.

- **Control interface:** Communicates with the RetroVue Python runtime using gRPC (`proto/retrovue/playout.proto`).
- **Output:** Streams decoded frames to the Renderer, which emits MPEG-TS per virtual channel.
- **Timing:** Synchronizes all output using the system-wide `MasterClock` for deterministic alignment.

---

## Component Overview

| Component             | Language        | Responsibility                                |
| --------------------- | --------------- | --------------------------------------------- |
| **RetroVue Core**     | Python          | Scheduling, orchestration, channel management |
| **RetroVue Renderer** | Python + ffmpeg | MPEG-TS encoding & transport                  |
| **Playout Engine**    | C++             | Decode, buffer, and align to MasterClock      |

Each component communicates over a documented API surface. The C++ playout engine **does not** implement scheduling logic — this remains in the RetroVue Core.

---

## Contracts & Interfaces

| Type    | Path/Location                  | Description                                      |
| ------- | ------------------------------ | ------------------------------------------------ |
| gRPC    | `proto/retrovue/playout.proto` | Control API: ChannelManager ↔ Playout (required) |
| Metrics | Prometheus `/metrics` endpoint | Channel state, frame gap telemetry               |
| Build   | `CMakeLists.txt`               | Defines `retrovue_playout` and dependencies      |

**Workflow:**  
_New features start by updating the contract_ (proto, metrics, etc.) in `docs/contracts/` and `proto/`, **before** implementing new functionality.

---

## Building & Running

**Build (Release)**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Run**

```bash
./build/retrovue_playout --channel 1 --port 8090
```

---

## Communication Model

- **Control API (gRPC):** Receives channel start/stop/update commands from the ChannelManager (Python client).
- **Frame Bus:** Shares decoded frames with the Renderer via a per-channel ring buffer.
- **Telemetry:** Publishes health and state metrics at the Prometheus `/metrics` endpoint.

---

## Development Roadmap

### Phase 1: Bring-Up

- Generate gRPC stubs (`playout.pb.cc` / `.h`) from `proto/retrovue/playout.proto`
- Scaffold `main.cpp` with gRPC service stub
- Implement in-memory frame queue (stub producer)
- Initialize Prometheus metrics

### Phase 2: Integration

- Implement decode loop using libavformat/libavcodec
- Connect frame output to Renderer (via pipe/TCP)
- Synchronize with MasterClock
- Add fallback logic (slate frames, retry loop on failure)

### Phase 3: Testing & CI

- Unit tests for gRPC and decode pipeline
- Integration test with RetroVue runtime
- Update documentation and contracts as needed

---

## Notes & House Rules

- This repository is **not** the owner of scheduling logic. Scheduling is _authoritative_ in the RetroVue Core (Python).
- Always treat Python → C++ interactions as client → server.
- Keep all timing and output strictly deterministic based on the MasterClock.
- All API and integration changes **must** follow the `docs/contracts/` workflow (contract first, then implementation).

---

_For further details, see:_

- [docs/README.md](README.md)
- [runtime/PlayoutRuntime.md](runtime/PlayoutRuntime.md)
- [developer/BuildAndDebug.md](developer/BuildAndDebug.md)
