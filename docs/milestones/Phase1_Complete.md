# Phase 1 Skeleton - Implementation Summary

## Overview

This document describes the minimal C++ skeleton implementation for the RetroVue Playout Engine, completing the first milestone of Phase 1 (Bring-up) as outlined in [PROJECT_OVERVIEW.md](../PROJECT_OVERVIEW.md).

## What was built

### Core service implementation

Three new source files implement the PlayoutControl gRPC service:

1. **`src/playout_service.h`**: Service class declaration
   - `PlayoutControlImpl` class inheriting from `PlayoutControl::Service`
   - Thread-safe state tracking using `std::mutex`
   - All four RPC method signatures defined

2. **`src/playout_service.cpp`**: Service implementation
   - `StartChannel`: Validates channel availability, tracks active channels
   - `UpdatePlan`: Hot-swaps plan handles for running channels
   - `StopChannel`: Gracefully removes channels from active set
   - `GetVersion`: Returns API version (1.0.0) matching proto contract
   - Comprehensive logging for all operations
   - Proper error handling with gRPC status codes

3. **`src/main.cpp`**: Server entry point
   - Command-line argument parsing (`--port`, `--address`, `--help`)
   - gRPC server builder with health check and reflection enabled
   - Clean startup banner with configuration summary
   - Exception handling for graceful error reporting

### Build integration

Updated `CMakeLists.txt` to compile the new executable:

- Added `retrovue_playout` target
- Links against `retrovue_playout_proto` (generated stubs)
- Includes `src/` and generated proto headers
- Produces `retrovue_playout.exe` (Windows) / `retrovue_playout` (Linux)

### Testing infrastructure

Created `scripts/test_server.py` to validate the service:

- Connects to gRPC server using Python stubs
- Exercises all four RPC methods in sequence
- Tests error conditions (non-existent channels)
- Provides clear pass/fail output

### Documentation

Added `docs/developer/QuickStart.md`:

- Build instructions (PowerShell and Bash)
- Run and test procedures
- Phase 1-3 implementation checklist
- Architecture diagram
- Debugging tips (gRPC reflection, Visual Studio)

## Implementation details

### State management

The skeleton uses a simple in-memory map to track active channels:

```cpp
std::unordered_map<int32_t, std::string> active_channels_;
```

- Key: `channel_id` from StartChannelRequest
- Value: `plan_handle` (updated by UpdatePlan)
- Protected by `channels_mutex_` for thread safety

### Error handling

All RPC methods return appropriate gRPC status codes:

- `OK`: Successful operation
- `ALREADY_EXISTS`: Attempting to start an active channel
- `NOT_FOUND`: Attempting to modify/stop non-existent channel

Responses include descriptive messages for debugging.

### Logging

All operations log to stdout with structured format:

```
[RpcMethod] Description: key=value, key2=value2
```

Example:

```
[StartChannel] Request received: channel_id=1, plan_handle=test-plan-001, port=8090
[StartChannel] Channel 1 started successfully
```

## Testing results

### Build verification

```powershell
PS> cmake --build build
...
retrovue_playout.vcxproj -> C:\...\retrovue_playout.exe
✅ Build succeeded (0 errors, 0 warnings)
```

### Runtime verification

```powershell
PS> .\build\Debug\retrovue_playout.exe --help
RetroVue Playout Engine

Usage: retrovue_playout [OPTIONS]
...
✅ Help output correct
```

### Service functionality

All RPC methods tested successfully:

- ✅ GetVersion: Returns "1.0.0"
- ✅ StartChannel: Accepts new channels
- ✅ UpdatePlan: Swaps plan handles
- ✅ StopChannel: Removes active channels
- ✅ Error handling: Rejects invalid operations with correct status codes

## What's NOT implemented (next steps)

### Remaining Phase 1 tasks

1. **In-memory frame queue**
   - Ring buffer for decoded frames
   - Producer stub for testing (no actual decode yet)
   - Frame metadata structure (PTS, DTS, duration)

2. **Prometheus metrics**
   - `/metrics` HTTP endpoint
   - `retrovue_playout_channel_state{channel="N"}`
   - `retrovue_playout_frame_gap_seconds{channel="N"}`

### Phase 2 requirements

All decode, MasterClock sync, and Renderer integration work remains for Phase 2.

## Contract compliance

The implementation strictly adheres to the proto contract:

- **API Version**: Matches `PLAYOUT_API_VERSION = "1.0.0"` from `playout.proto`
- **Message schemas**: All request/response fields honored
- **Service interface**: All four RPCs implemented as specified
- **Status codes**: Proper gRPC error semantics

## Integration readiness

The skeleton is ready for integration with the Python ChannelManager:

1. Generate Python stubs: `python scripts/generate_stubs.py`
2. Start the playout engine: `./build/retrovue_playout --port 50051`
3. Import stubs in Python: `from retrovue import playout_pb2, playout_pb2_grpc`
4. Create channel: `grpc.insecure_channel("localhost:50051")`

Example Python client code:

```python
import grpc
from retrovue import playout_pb2, playout_pb2_grpc

channel = grpc.insecure_channel("localhost:50051")
stub = playout_pb2_grpc.PlayoutControlStub(channel)

request = playout_pb2.StartChannelRequest(
    channel_id=1,
    plan_handle="my-plan",
    port=8090
)
response = stub.StartChannel(request)
print(response.message)
```

## File manifest

New files created:

```
src/
├── main.cpp                 (87 lines)
├── playout_service.h        (56 lines)
└── playout_service.cpp      (131 lines)

scripts/
└── test_server.py           (138 lines)

docs/developer/
└── QuickStart.md            (243 lines)

PHASE1_SKELETON.md           (this file)
```

Modified files:

```
CMakeLists.txt               (+13 lines: retrovue_playout target)
```

## Build artifacts

```
build/
├── Debug/
│   └── retrovue_playout.exe     # Main executable
├── generated/
│   └── retrovue/
│       ├── playout.pb.h         # Proto headers (generated)
│       ├── playout.pb.cc
│       ├── playout.grpc.pb.h
│       └── playout.grpc.pb.cc
└── ...
```

## Validation checklist

- ✅ Compiles without errors or warnings
- ✅ All four RPC methods implemented
- ✅ Thread-safe state management
- ✅ Proper gRPC error codes
- ✅ Command-line argument parsing
- ✅ gRPC health check enabled
- ✅ gRPC reflection enabled
- ✅ Python test client validates all methods
- ✅ Documentation updated
- ✅ Follows RetroVue code conventions

## Next development session

To continue with Phase 1, implement the frame queue:

1. Create `src/frame_queue.h` and `src/frame_queue.cpp`
2. Define frame metadata structure (timestamp, asset ID, duration)
3. Implement ring buffer with configurable capacity (default 90 frames)
4. Add producer thread that generates test frames
5. Update `playout_service.cpp` to initialize/destroy queues per channel

See [PlayoutRuntime.md](../runtime/PlayoutRuntime.md) for frame queue specifications.

## References

- [PROJECT_OVERVIEW.md](../PROJECT_OVERVIEW.md) - Phase 1 roadmap
- [QuickStart.md](../developer/QuickStart.md) - Build and run guide
- [PlayoutRuntime.md](../runtime/PlayoutRuntime.md) - Runtime model
- [playout.proto](../../proto/retrovue/playout.proto) - Canonical API contract

