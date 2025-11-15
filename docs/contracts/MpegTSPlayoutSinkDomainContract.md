_Related: [MPEG-TS Playout Sink Domain](../domain/MpegTSPlayoutSinkDomain.md) • [Playout Engine Contract](PlayoutEngineContract.md) • [Video File Producer Contract](VideoFileProducerDomainContract.md) • [Architecture Overview](../architecture/ArchitectureOverview.md)_

# Contract — MPEG-TS Playout Sink Domain

Status: Enforced

## Purpose

This document defines the **behavioral and testing contract** for the MPEG-TS Playout Sink subsystem in the RetroVue Air playout engine. The MPEG-TS Playout Sink is responsible for consuming decoded frames from a `FrameRingBuffer`, encoding them into H.264, wrapping them in MPEG-TS format, and streaming them over TCP socket. The sink owns its own timing loop that continuously queries MasterClock and compares with frame PTS to determine when to output frames.

This contract establishes:

- **Functional guarantees** for frame consumption, encoding, muxing, and streaming operations
- **Performance expectations** for encoding throughput, latency, and resource utilization
- **Error recovery procedures** for encoding failures, network errors, and buffer underruns
- **Lifecycle guarantees** for start, stop, and teardown operations
- **Verification criteria** for automated testing and continuous validation

The MPEG-TS Playout Sink must operate deterministically, own its timing loop (continuously querying MasterClock), and provide observable statistics for all operational states. All frames consumed are decoded and ready for encoding. The sink pulls time from MasterClock (MasterClock never pushes ticks).

---

## Scope

This contract enforces guarantees for the following MPEG-TS Playout Sink components:

### 1. MpegTSPlayoutSink (Core Component)

**Purpose**: Manages the sink worker thread lifecycle (owns timing loop) and coordinates frame consumption, encoding, and streaming. Performs encoding, muxing, and network output internally. The sink continuously queries MasterClock and compares with frame PTS to determine when to output frames.

**Contract**:

```cpp
class MpegTSPlayoutSink : public IPlayoutSink {
public:
    MpegTSPlayoutSink(
        const SinkConfig& config,
        buffer::FrameRingBuffer& input_buffer,
        std::shared_ptr<timing::MasterClock> master_clock
    );
    
    bool start() override;
    void stop() override;
    bool isRunning() const override;
    
    uint64_t getFramesSent() const override;
    uint64_t getFramesDropped() const override;
    uint64_t getLateFrames() const override;
    
    uint64_t getEncodingErrors() const;
    uint64_t getNetworkErrors() const;
    uint64_t getBufferEmptyCount() const;
};
```

**Guarantees**:

- `start()` returns false if already running (idempotent start prevention)
- `start()` starts worker thread that owns the timing loop
- `stop()` stops worker thread and blocks until thread exits (safe to call from any thread)
- Destructor automatically calls `stop()` if sink is still running
- Statistics (`getFramesSent()`, `getFramesDropped()`, `getLateFrames()`) are thread-safe
- All frames consumed are decoded (YUV420 format, ready for encoding)
- Sink owns timing loop: continuously queries MasterClock and compares with frame PTS
- MasterClock never pushes ticks (sink pulls time from MasterClock)

### 2. Internal Encoder Subsystem

**Purpose**: Encapsulated libx264 and FFmpeg-based components that perform encoding, muxing, and network output.

**Components**:

- **Encoder** (libx264): Encodes decoded YUV420 frames into H.264 NAL units
- **Muxer** (libavformat): Packages H.264 packets into MPEG-TS transport stream format
- **Network Output**: Sends MPEG-TS packets to TCP socket (server mode, accepts one client)

**Guarantees**:

- All encoding operations are internal to MpegTSPlayoutSink
- External components only interact with MPEG-TS stream output
- Encoder subsystem handles H.264 encoding, MPEG-TS muxing, and network streaming
- Encoder subsystem reports encoding errors for recovery handling
- Encoder subsystem reports network errors for recovery handling

---

## Test Environment Setup

All MPEG-TS Playout Sink contract tests must run in a controlled environment with the following prerequisites:

### Required Resources

| Resource              | Specification                                      | Purpose                            |
| --------------------- | -------------------------------------------------- | ---------------------------------- |
| Test Decoded Frames   | YUV420 1080p30, 10s duration, monotonic PTS       | Standard frame source for testing  |
| FrameRingBuffer       | 60-frame capacity (default)                        | Frame staging buffer (decoded frames only) |
| MasterClock Mock      | Monotonic, microsecond precision                   | Controlled timing source           |
| Internal Encoder      | libx264 + libavformat (optional for stub mode)    | Real video encode (if available)   |
| Network Test Harness  | TCP client for MPEG-TS validation                | Stream validation                  |
| VLC Media Player      | For MPEG-TS playback verification                 | Stream playability verification    |

### Environment Variables

```bash
RETROVUE_SINK_TARGET_FPS=30.0        # Target frame rate
RETROVUE_SINK_BITRATE=5000000        # Encoding bitrate (5 Mbps)
RETROVUE_SINK_GOP_SIZE=30            # GOP size (1 second at 30fps)
RETROVUE_SINK_PORT=9000              # TCP server port
RETROVUE_SINK_STUB_MODE=false        # Use real encode (true for stub mode)
RETROVUE_SINK_BUFFER_SIZE=60         # FrameRingBuffer capacity
```

### Pre-Test Validation

Before running contract tests, verify:

1. ✅ FrameRingBuffer operations pass smoke tests
2. ✅ MasterClock advances monotonically
3. ✅ Test decoded frames are valid YUV420 format
4. ✅ Internal encoder subsystem initializes correctly (if using real encode mode)
5. ✅ Network test harness can connect to TCP socket
6. ✅ VLC can play MPEG-TS streams (for FE-015 / PE-005 playback sanity verification)
7. ✅ Stub mode consumes frames correctly (fallback validation)

---

## Functional Expectations

The MPEG-TS Playout Sink must satisfy the following behavioral guarantees:

### FE-001: Sink Lifecycle

**Rule**: Sink must support clean start, stop, and teardown operations.

**Expected Behavior**:

- Sink initializes in stopped state with zero frames sent
- `start()` returns true on first call, false if already running
- `start()` starts worker thread that owns the timing loop
- `stop()` stops worker thread and blocks until thread exits (no hanging threads)
- `stop()` is idempotent (safe to call multiple times)
- Destructor automatically stops sink if still running

**Test Criteria**:

- ✅ Construction: `isRunning() == false`, `getFramesSent() == 0`
- ✅ Start: `start() == true`, `isRunning() == true`
- ✅ Start twice: Second `start()` returns false
- ✅ Worker thread: Worker thread starts and runs timing loop
- ✅ Stop: `stop()` stops worker thread and blocks until thread exits, `isRunning() == false`
- ✅ Stop idempotent: Multiple `stop()` calls are safe
- ✅ Destructor: Sink stops automatically on destruction

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_001_SinkLifecycle, FE_001_DestructorStopsSink)

---

### FE-002: Pulls Frames in Order

**Rule**: Sink must pull decoded frames from buffer in order (FIFO).

**Expected Behavior**:

- Sink pops frames from buffer in the order they were pushed
- Sink maintains frame order through encoding and output
- Sink does not skip frames unless they are late (see FE-003)
- Frame PTS values increase monotonically in output stream

**Test Criteria**:

- ✅ Frame order: Frames are output in the same order as pushed to buffer
- ✅ PTS monotonicity: Output frame PTS values increase monotonically
- ✅ No frame reordering: Sink does not reorder frames during encoding
- ✅ FIFO compliance: Buffer pop order matches push order

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_002_PullsFramesInOrder, FE_002_FramePTSMonotonicity)

---

### FE-003: Obeys Master Clock

**Rule**: Sink must own timing loop, query MasterClock, and output frames when PTS is due.

**Expected Behavior**:

- Sink owns timing loop (worker thread continuously running)
- Sink queries `MasterClock::now_utc_us()` in timing loop
- Sink compares frame PTS with MasterClock time: if `frame.pts_us <= master_time_us`, output frame
- Sink sleeps when ahead of schedule (frame PTS in future)
- Sink drops frames when behind schedule (frame PTS overdue)
- Sink maintains real-time output pacing aligned to MasterClock
- MasterClock never pushes ticks (sink pulls time from MasterClock)

**Test Criteria**:

- ✅ Timing loop: Sink worker thread continuously runs timing loop
- ✅ Clock query: Sink queries MasterClock in timing loop (`master_clock_->now_utc_us()`)
- ✅ Frame selection: Sink compares frame PTS with MasterClock time to determine when to output
- ✅ Ahead of schedule: Sink sleeps when frame PTS is in future
- ✅ Behind schedule: Sink drops frames when frame PTS is overdue
- ✅ Timing accuracy: Frame output timing matches MasterClock time (conforms to FE-017 jitter/drift bounds)
- ✅ Late frame detection: Sink detects late frames correctly (threshold: 33ms at 30fps)
- ✅ No tick() calls: MasterClock never calls tick() on sink (sink pulls time)

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_003_ObeysMasterClock, FE_003_DropsLateFrames)

---

### FE-004: Encodes Valid H.264 Frames

**Rule**: Sink must encode decoded frames into valid H.264 NAL units.

**Expected Behavior**:

- Encoder produces valid H.264 NAL units (SPS, PPS, IDR, P, B frames)
- Encoder maintains encoder state correctly (reference frames, GOP structure)
- Encoded bitstream is decodable by standard H.264 decoders
- Encoder respects bitrate and GOP size configuration

**Test Criteria**:

- ✅ Valid H.264: Encoded output is valid H.264 bitstream (parsable by libavcodec)
- ✅ SPS/PPS: Encoder generates SPS/PPS when needed (IDR frames)
- ✅ Decodable: Encoded frames can be decoded back to YUV420 (round-trip test)
- ✅ Bitrate compliance: Encoded bitrate matches configuration (within 10% tolerance)
- ✅ GOP structure: IDR frames appear at configured GOP interval

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_004_EncodesValidH264)

---

### FE-005: Error Detection and Classification

**Rule**: The MPEG-TS Playout Sink must detect and classify all internal and external error conditions that would compromise output correctness, timing integrity, or device stability.

**Error Classes**:

**Recoverable Errors**:

- Late frames (within discardable threshold)
- Temporary upstream starvation
- PID discontinuity in input
- Single PCR miss within allowable recovery range
- Transient thread scheduling delays

**Degraded-Mode Errors**:

- Sustained starvation (above N ms)
- Repeated late frames beyond discard window
- PCR recovery requiring discontinuity flag insertion
- Buffer overrun/underrun during steady load

**Fault / Unrecoverable Errors**:

- Corrupted frame memory
- Decoder-critical invariant failure
- Long-term MasterClock desynchronization
- Repeated internal exceptions
- TS muxer producing invalid packets

**Pass Criteria**:

- ✅ Error detection: Sink successfully detects and classifies all errors
- ✅ Error classification: Sink does not misclassify unrecoverable errors as recoverable
- ✅ Recoverable handling: Recoverable errors increment counters but do not force a state transition
- ✅ Error logging: All errors generate expected log/event/audit entries

**Fail Criteria**:

- ❌ A recoverable error causes a sink fault
- ❌ An unrecoverable error is ignored or remains unreported
- ❌ Error does not generate the expected log/event/audit entry

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_005_ErrorDetectionAndClassification)

---

### FE-006: Handles Empty Buffer (Waits, No Crash)

**Rule**: Sink must handle empty buffer gracefully by waiting, not crashing.

**Expected Behavior**:

- When buffer is empty in timing loop:
  - Sink increments `buffer_empty_count_` statistic
  - Sink applies underflow policy (frame freeze, black frame, or skip)
  - Sink waits 10ms (`kSinkWaitUs`)
  - Sink retries peek on next loop iteration
- Sink never crashes or throws exception on empty buffer
- Sink resumes normal operation when buffer has frames

**Test Criteria**:

- ✅ Empty buffer detection: `getBufferEmptyCount() > 0` when buffer is empty
- ✅ Underflow policy: Sink applies underflow policy (frame freeze, black frame, or skip)
- ✅ Wait timing: Sink waits ~10ms before retry (within 2ms tolerance)
- ✅ No crash: Sink does not crash or throw exception on empty buffer
- ✅ Recovery: Sink resumes frame consumption when buffer has frames
- ✅ Statistics accuracy: `getBufferEmptyCount()` accurately tracks empty buffer events

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_006_HandlesEmptyBuffer)

---

### FE-007: Handles Buffer Overrun (Drops, No Crash)

**Rule**: Sink must handle buffer overrun gracefully by dropping late frames, not crashing.

**Expected Behavior**:

- When sink is behind schedule (frames are late):
  - Sink drops late frames (more than `kLateThresholdUs` late)
  - Sink increments `frames_dropped_` and `late_frames_` counters
  - Sink continues to next frame (attempts to catch up)
- Sink never crashes or throws exception on buffer overrun
- Sink maintains real-time output (no buffering of late frames)

**Test Criteria**:

- ✅ Late frame detection: Sink detects when frames are late (deadline in past)
- ✅ Frame dropping: Sink drops late frames when behind schedule
- ✅ Statistics tracking: `GetFramesDropped()` and `GetLateFrames()` increment correctly
- ✅ No crash: Sink does not crash or throw exception on buffer overrun
- ✅ Real-time output: Sink maintains real-time output pacing (no buffering)

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_007_HandlesBufferOverrun)

---

### FE-008: Properly Reports Stats (Frames Sent, Frames Dropped, Late Frames)

**Rule**: Sink statistics must accurately reflect operational state.

**Expected Behavior**:

- `getFramesSent()`: Counts only successfully sent frames
- `getFramesDropped()`: Counts frames dropped due to lateness
- `getLateFrames()`: Counts late frame events
- `getEncodingErrors()`: Counts encoding failures
- `getNetworkErrors()`: Counts network send failures
- `getBufferEmptyCount()`: Counts buffer empty events
- Statistics are updated atomically (thread-safe)
- Statistics are safe to read from any thread

**Test Criteria**:

- ✅ Frame counting: `getFramesSent()` matches actual frames sent to output
- ✅ Drop tracking: `getFramesDropped()` increments on each late frame drop
- ✅ Late frame tracking: `getLateFrames()` increments on each late frame event
- ✅ Error tracking: `getEncodingErrors()` and `getNetworkErrors()` track failures
- ✅ Thread safety: Statistics can be read from any thread without race conditions
- ✅ Accuracy: Statistics reflect actual operational state

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_008_ProperlyReportsStats)

---

### FE-009: Nonblocking Write

**Rule**: Sink must handle non-blocking socket writes gracefully by queuing packets when socket returns `EAGAIN`/`EWOULDBLOCK`.

**Expected Behavior**:

- When socket write returns `EAGAIN`/`EWOULDBLOCK`, packet is queued to output queue
- Worker thread does not block on network writes
- Sink continues running even when socket buffer is full
- Packets are eventually sent when socket becomes writable

**Test Criteria**:

- ✅ Non-blocking writes: Sink handles `EAGAIN`/`EWOULDBLOCK` without blocking
- ✅ Packet queuing: Packets are queued when socket is not writable
- ✅ Worker thread: Worker thread continues running (does not deadlock)
- ✅ Sink stability: Sink remains running during network backpressure

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_009_NonblockingWrite)

---

### FE-010: Queue Overflow

**Rule**: Sink must handle output queue overflow gracefully by dropping older packets, not crashing.

**Expected Behavior**:

- When output queue reaches capacity, older packets are dropped
- Sink continues operation (does not crash)
- New packets continue to be queued
- Queue size remains bounded

**Test Criteria**:

- ✅ Queue overflow: Sink handles queue overflow without crashing
- ✅ Packet dropping: Older packets are dropped when queue is full
- ✅ Sink stability: Sink continues running during queue overflow
- ✅ Queue bounded: Queue size does not grow unbounded

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_010_QueueOverflow)

---

### FE-011: Client Disconnect

**Rule**: Sink must handle client disconnect gracefully and continue waiting for new connections.

**Expected Behavior**:

- When client disconnects, sink detects disconnect
- Sink tears down muxer and encoder resources
- Sink continues running (waiting for new client)
- New client can connect and streaming resumes

**Test Criteria**:

- ✅ Disconnect detection: Sink detects client disconnect
- ✅ Resource cleanup: Muxer and encoder are torn down on disconnect
- ✅ Sink continues: Sink remains running after disconnect
- ✅ Reconnection: New client can connect and streaming resumes

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_011_ClientDisconnect)

---

### FE-012: Error → Sink Status Mapping

**Rule**: Each error class from FE-005 must map deterministically to the correct sink status and expose that status externally through the API/telemetry interface.

**Status Values**:

**RUNNING**:

- Recoverable errors encountered but successfully handled
- Output timing remains stable or corrected by PCR shaping

**DEGRADED**:

- Degraded-mode errors encountered
- Output remains valid MPEG-TS
- Clock drift or PCR correction operating at enhanced levels
- Upstream should be alerted but downstream playback must remain intact

**FAULTED**:

- Unrecoverable errors encountered
- Output stability or spec compliance cannot be guaranteed
- Sink halts output or enters safe "null fill" mode depending on env config

**Pass Criteria**:

- ✅ Status mapping: Status reflects the highest severity error currently active
- ✅ Status elevation: Once elevated (RUNNING → DEGRADED → FAULTED), the status:
  - may return to RUNNING from DEGRADED if recovery rules allow
  - must never silently clear FAULTED without an explicit reset
- ✅ Status exposure: Status is exposed through public API/telemetry interface
- ✅ Deterministic mapping: Error class maps deterministically to correct status

**Fail Criteria**:

- ❌ Incorrect status for given error class
- ❌ Status fails to propagate through public API
- ❌ Sink oscillates between states without conditions changing

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_012_ErrorToSinkStatusMapping)

---

### FE-013: Encodes Real Decoded Frames

**Rule**: Sink must encode real decoded frames (from VideoFileProducer) into valid H.264 packets.

**Expected Behavior**:

- Sink receives decoded frames from VideoFileProducer
- Frames are encoded into H.264 NAL units
- Encoded packets are muxed into MPEG-TS format
- Output stream contains valid MPEG-TS packets with sync bytes

**Test Criteria**:

- ✅ Real encoding: Sink encodes real decoded frames (not stub)
- ✅ H.264 output: Encoded output contains H.264 NAL units
- ✅ MPEG-TS format: Output contains valid MPEG-TS packets (0x47 sync byte)
- ✅ Substantial data: Output contains substantial data (indicating encoding occurred)

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_013_EncodesRealDecodedFrames)

---

### FE-014: Generates Increasing PTS in MPEG-TS Output

**Rule**: MPEG-TS output must have monotonically increasing PTS values.

**Expected Behavior**:

- PTS values in MPEG-TS packets increase monotonically
- No PTS values decrease or repeat (except at wrap-around)
- PTS ordering matches input frame ordering

**Test Criteria**:

- ✅ PTS monotonicity: PTS values increase monotonically in output
- ✅ No PTS regression: PTS values never decrease
- ✅ PTS ordering: PTS ordering matches input frame ordering

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_014_GeneratesIncreasingPTSInMpegTSOutput)

---

### FE-015: Output Is Readable by FFprobe

**Rule**: MPEG-TS output must be readable by FFmpeg/FFprobe (valid stream format).

**Expected Behavior**:

- Output stream can be opened by `avformat_open_input()`
- Stream info can be found by `avformat_find_stream_info()`
- Stream contains valid video stream with correct codec parameters
- Codec parameters (width, height, codec_id) are correct

**Test Criteria**:

- ✅ FFmpeg readable: Output can be opened by `avformat_open_input()`
- ✅ Stream info: Stream info can be found
- ✅ Video stream: Stream contains valid video stream
- ✅ Codec parameters: Codec parameters are correct (H.264, valid dimensions)

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_015_OutputIsReadableByFFprobe)

---

### FE-016: Frame Timing Is Preserved Through Encoding and Muxing

**Rule**: Frame PTS values must be preserved through encoding and muxing with acceptable tolerance.

**Expected Behavior**:

- Frame PTS values are preserved through encoding pipeline
- MPEG-TS PTS values correspond to input frame PTS (within tolerance)
- Frame rate is maintained in output stream
- Timing accuracy is within tolerance (e.g., 50ms)

**Test Criteria**:

- ✅ PTS preservation: Frame PTS values are preserved (within tolerance)
- ✅ Frame rate: Output frame rate matches input frame rate
- ✅ Timing accuracy: Timing accuracy is within tolerance (50ms)

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_016_FrameTimingIsPreservedThroughEncodingAndMuxing)

---

### FE-017: Real-Time Output Timing Stability

**Rule**: The MPEG-TS Playout Sink must produce transport stream packets at a stable, real-time rate relative to the MasterClock, within tolerances achievable on non-real-time operating systems, while preserving PCR correctness and output compliance.

**1. Timing Jitter Requirements**:

**Steady-State Jitter**:

- During normal operation under typical CPU load:
  - Output packet timing shall remain within ±8 ms of ideal real-time scheduling
  - Rationale: ±8 ms is achievable under Linux with real-time priority threads and matches typical playout jitter budgets for software-based systems

**Short Burst Tolerance**:

- The system may experience rare jitter spikes due to OS scheduling or GC/allocator stalls
- Allowed:
  - Occasional spikes up to ±15 ms
  - No more than 1 spike per 5 seconds
  - Must self-correct by the next 3 packets

**Sustained Drift**:

- The sink must prevent accumulated drift from exceeding:
  - 20 ms deviation from real-time over any rolling 1-second window
  - If drift approaches this threshold, PCR correction and buffer shaping must resynchronize output

**2. PCR Correctness Requirements**:

- Even if wall-clock jitter occurs, PCR output must remain compliant:
  - PCR slope error must remain within ±500 ns per PCR interval (DVB/ATSC limit)
  - Note: Actual measurement may use coarser resolution (e.g., microsecond-level) but must ensure compliance with the ±500 ns limit when validated
  - PCR discontinuity_flag must be inserted if slope correction exceeds automatic correction limits
  - PCR correctness is the primary compliance requirement; wall-clock jitter is secondary

**3. Late Frame Handling**:

- If the sink receives input frames later than their scheduled playout time:
  - The sink must drop or fast-forward decode (per FE-007 / FE-008 / EH-005) to return to real-time
  - These drops do not count as timing jitter failures
  - They do count in FE-007 / FE-008 / EH-005 late-frame metrics

**Current Test Implementation** (as verified by `FE_017_RealTimeOutputTimingStability`):

The test verifies PCR correctness as the primary compliance requirement:

- ✅ PCR intervals: PCR intervals are within acceptable range (≥ 1800 ticks / 20ms, ≤ 9000 ticks / 100ms)
- ✅ PCR monotonicity: PCR values increase monotonically
- ✅ First PCR interval skipped: First PCR interval (i=1) is skipped as initialization artifact
- ✅ PCR slope compliance: PCR intervals remain within DVB/ATSC limits (20-100ms per ISO/IEC 13818-1)

**Note**: The test focuses on PCR correctness rather than detailed wall-clock jitter measurements. The jitter/drift requirements above represent design goals, but the current test validates PCR compliance as the primary metric.

**Pass Criteria** (as tested):

- ✅ PCR intervals: ≥ 1800 (20ms in 90kHz units), ≤ 9000 (100ms in 90kHz units)
- ✅ PCR monotonicity: PCR values increase monotonically
- ✅ First interval skipped: First PCR diff is ignored (initialization artifact)
- ✅ Multiple PCR packets: Test collects at least 10 PCR packets for validation

**Fail Criteria** (as tested):

- ❌ PCR interval too short: < 1800 ticks (< 20ms)
- ❌ PCR interval too long: > 9000 ticks (> 100ms)
- ❌ PCR non-monotonic: PCR values decrease or repeat

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_017_RealTimeOutputTimingStability)

---

### FE-018: PTS/DTS Integrity

**Rule**: PTS/DTS values must be monotonic and satisfy DTS ≤ PTS.

**Expected Behavior**:

- PTS values increase monotonically
- DTS values increase monotonically
- DTS ≤ PTS for all packets
- No PTS/DTS regression

**Test Criteria**:

- ✅ PTS monotonicity: PTS values increase monotonically
- ✅ DTS monotonicity: DTS values increase monotonically
- ✅ DTS ≤ PTS: DTS ≤ PTS for all packets
- ✅ No regression: No PTS/DTS values decrease

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_018_PTS_DTS_Integrity)

---

### FE-019: PCR Cadence

**Rule**: PCR (Program Clock Reference) packets must appear at correct intervals per ISO/IEC 13818-1.

**Expected Behavior**:

- PCR packets appear at regular intervals
- Real PCR cadence:
  - ~20ms minimum (rare)
  - ~66ms typical
  - ≤100ms maximum (ISO/IEC 13818-1: max PCR interval 100ms)
- PCR values increase monotonically
- PCR cadence is maintained throughout stream

**Test Criteria**:

- ✅ PCR intervals: PCR intervals are within 20-100ms range (ISO/IEC 13818-1 compliant)
- ✅ PCR monotonicity: PCR values increase monotonically
- ✅ PCR cadence: PCR cadence is maintained (≥ 20ms, ≤ 100ms)
- ✅ First PCR interval skipped: First PCR interval is ignored (initialization artifact)

**Current Test Criteria** (as implemented):
- PCR intervals: ≥ 1800 (20ms in 90kHz units), ≤ 9000 (100ms in 90kHz units)
- First PCR interval (i=1) is skipped as it is always garbage (initialization artifact)
- Note: This range accounts for real-time encoding variations, VBR mode behavior, and ISO/IEC 13818-1 specification requirements

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_019_PCR_Cadence)

---

### FE-020: Fault Mode Behavior & Latching

**Rule**: Fault states must remain latched until the sink is explicitly reset, to ensure upstream controllers are aware of unrecoverable issues and can coordinate restarts without race conditions.

**Required Behavior**:

**Fault Latching**:

- Upon entering FAULTED:
  - latches active fault code
  - halts or safe-fills output stream
  - stops accepting new frames
  - reports via telemetry immediately

**Fault Isolation**:

- Sink must not reinitialize internal mux/timing loops automatically
- No auto-recover behavior unless explicitly enabled (off by default)

**Reset Requirements**:

- Only the following actions clear fault state:
  - Explicit `reset()` API call
  - Full teardown + re-instantiate of the sink object

**Auditability**:

- A full fault report must be available, including:
  - fault type
  - last valid PCR/PTS values
  - last n error events
  - uptime at fault time
  - input/output byte counters

**Current Test Implementation** (as verified by `FE_020_FaultModeBehaviorAndLatching`):

The test currently verifies basic state consistency and teardown behavior:

- ✅ State consistency: Sink maintains consistent state (running or not running)
- ✅ Full teardown clears state: Re-instantiated sink starts in clean state
- ✅ Basic fault isolation: Sink does not auto-recover (verified via state consistency)

**Note**: The test is currently a placeholder that verifies basic state management. Full fault injection, latching, and auditability testing requires additional API support (e.g., `reset()`, `getFaultReport()`, explicit fault injection methods).

**Pass Criteria** (as tested):

- ✅ State consistency: Sink state is consistent (no invalid states)
- ✅ Clean teardown: Full teardown and re-instantiation results in clean state
- ✅ Sink stability: Sink remains stable during test execution

**Fail Criteria** (as tested):

- ❌ State inconsistency: Sink enters invalid state
- ❌ Teardown failure: Re-instantiated sink does not start cleanly

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_020_FaultModeBehaviorAndLatching)

---

### FE-021: Encoder Stall Recovery

**Rule**: Sink must detect and recover from encoder stalls.

**Expected Behavior**:

- Sink detects encoder stalls (no output for extended period)
- Sink continues operation (does not deadlock)
- Sink recovers from stall when encoder resumes
- Worker thread remains responsive

**Current Test Implementation** (as verified by `FE_021_EncoderStallRecovery`):

The test verifies that the sink continues operating even with slow writes:

- ✅ No deadlock: Sink does not deadlock during slow writes
- ✅ Worker responsiveness: Worker thread remains responsive
- ✅ Sink stability: Sink continues running during slow write conditions

**Note**: The test currently verifies basic stall resilience. Full stall detection and recovery testing requires additional instrumentation (e.g., write callback injection, stall detection metrics).

**Pass Criteria** (as tested):

- ✅ Sink continues running: Sink remains running during slow write conditions
- ✅ No deadlock: Worker thread does not block indefinitely
- ✅ Basic resilience: Sink handles slow writes gracefully

**Fail Criteria** (as tested):

- ❌ Sink deadlocks: Worker thread blocks indefinitely
- ❌ Sink stops: Sink stops running unexpectedly

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_021_EncoderStallRecovery)

---

### FE-022: Queue Invariants

**Rule**: Output queue must maintain invariants during overflow and backpressure.

**Expected Behavior**:

- Queue size is bounded (does not grow unbounded)
- Queue overflow handling drops packets correctly
- Queue remains stable during backpressure
- Sink continues operation during queue overflow

**Current Test Implementation** (as verified by `FE_022_QueueInvariants`):

The test verifies queue stability during overflow conditions:

- ✅ Queue bounded: Queue size remains bounded (does not grow unbounded)
- ✅ Sink stability: Sink continues operation during queue overflow
- ✅ Overflow handling: Queue overflow is handled without crashing

**Note**: The test verifies basic queue invariants. Detailed packet drop counting and queue size metrics may require additional API support.

**Pass Criteria** (as tested):

- ✅ Sink continues running: Sink remains running during queue overflow
- ✅ Queue bounded: Queue size does not grow unbounded
- ✅ No crash: Sink handles overflow without crashing

**Fail Criteria** (as tested):

- ❌ Sink crashes: Sink crashes during queue overflow
- ❌ Queue unbounded: Queue grows unbounded
- ❌ Sink stops: Sink stops running unexpectedly

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_022_QueueInvariants)

---

### FE-023: TS Packet Alignment Preserved

**Rule**: TS packets must be written only on 188-byte boundaries.

**Expected Behavior**:

- All TS packets are 188 bytes
- Every 188 bytes contains sync byte (0x47)
- No partial packets in output stream
- Packet alignment is preserved through network I/O

**Current Test Implementation** (as verified by `FE_023_TSPacketAlignmentPreserved`):

The test verifies TS packet alignment by checking sync byte positions:

- ✅ Sync byte alignment: Every 188 bytes contains sync byte (0x47)
- ✅ Packet alignment: TS packets are aligned on 188-byte boundaries
- ✅ Multiple packets: Test verifies alignment across multiple packets (≥ 10 packets)

**Note**: The test collects data in small chunks to verify alignment is preserved through network I/O. A remainder at the end (not a multiple of 188) is acceptable if it's the last partial chunk.

**Pass Criteria** (as tested):

- ✅ Sync byte alignment: Every 188-byte boundary has sync byte (0x47)
- ✅ Multiple packets: At least 10 aligned TS packets are verified
- ✅ No misalignment: No misaligned packets detected

**Fail Criteria** (as tested):

- ❌ Misaligned packets: Sync byte (0x47) not found at 188-byte boundaries
- ❌ Insufficient packets: Less than 10 aligned packets found

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` (FE_023_TSPacketAlignmentPreserved)

---

## Performance Expectations

The MPEG-TS Playout Sink must meet the following performance targets:

### PE-001: Encoding Throughput

**Target**: Sink must encode and output frames at or above `target_fps` rate.

**Metrics**:

- **Stub mode**: ≥ `target_fps` frames/second (e.g., ≥ 30 fps for 30 fps target)
- **Real encode mode**: ≥ `target_fps` frames/second for 1080p30 H.264 encoding
- **4K content**: ≥ 30 frames/second with hardware acceleration enabled

**Measurement**:

- Run sink for 10 seconds
- Measure `getFramesSent() / elapsed_time`
- Verify throughput ≥ `target_fps × 0.95` (5% tolerance)
- Verify all frames are encoded and sent

**Test Files**: `tests/test_performance.cpp`

---

### PE-002: Frame Encoding Latency

**Target**: Frame encoding latency (from buffer pop to MPEG-TS output) must be bounded.

**Metrics**:

- **Stub mode**: Frame consumption latency < 1ms (p95)
- **Real encode mode**: Frame encoding latency < 33ms (p95) for 1080p30 H.264
- **4K content**: Frame encoding latency < 50ms (p95) with hardware acceleration

**Measurement**:

- Measure time from buffer pop to MPEG-TS packet send completion
- Calculate p95 latency over 1000 frames
- Verify p95 latency < target threshold

**Test Files**: `tests/test_performance.cpp`

---

### PE-003: Memory Utilization

**Target**: Sink memory usage must be bounded.

**Metrics**:

- **Per-channel memory**: < 150 MB (encoder buffers + muxer overhead)
- **Internal encoder overhead**: < 50 MB per channel
- **Total memory**: < 200 MB per channel

**Measurement**:

- Run sink for 60 seconds
- Measure peak memory usage (RSS)
- Verify memory < threshold

**Test Files**: `tests/test_performance.cpp`

---

### PE-004: CPU Utilization

**Target**: Sink CPU usage must be reasonable.

**Metrics**:

- **Stub mode**: < 5% CPU per channel (single core)
- **Real encode mode (1080p30)**: < 40% CPU per channel (single core)
- **Real encode mode (4K)**: < 60% CPU per channel with hardware acceleration

**Measurement**:

- Run sink for 60 seconds
- Measure average CPU usage (single core)
- Verify CPU usage < threshold

**Test Files**: `tests/test_performance.cpp`

---

### PE-005: Network Output Rate

**Target**: Network output must maintain MPEG-TS packet delivery rate.

**Metrics**:

- **TCP mode**: Packet send rate ≥ `target_fps` packets/second
- **Non-blocking writes**: Network writes never block (EAGAIN/EWOULDBLOCK handled)
- **Client disconnect handling**: Sink tears down muxer and waits for new connection

**Measurement**:

- Run sink for 10 seconds
- Measure network packet send rate
- Verify send rate ≥ `target_fps × 0.95` (5% tolerance)
- Verify non-blocking writes (no blocking on network backpressure)

**Test Files**: `tests/test_performance.cpp`

---

## Error Handling

The MPEG-TS Playout Sink must handle the following error conditions:

### EH-001: Encoder Initialization Failure

**Condition**: Internal encoder subsystem initialization fails (libx264 not available, codec error, etc.).

**Expected Behavior**:

- Sink logs error: `"Failed to initialize encoder, falling back to stub mode"`
- Sink sets `config.stub_mode = true`
- Sink continues with stub frame consumption
- Sink does not crash or stop

**Test Criteria**:

- ✅ Encoder failure: Sink continues operation in stub mode
- ✅ Error is logged to stderr
- ✅ No crash or exception thrown
- ✅ Stub mode consumes frames correctly

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`

---

### EH-002: Encoding Error (Encoder Failure)

**Condition**: Internal encoder subsystem reports encoding error (codec error, frame encoding failure).

**Expected Behavior**:

- Sink logs error: `"Encoding errors: N"`
- Sink increments `stats.encoding_errors`
- Sink skips frame and continues (allows recovery)
- Sink does not stop (allows recovery)
- Sink continues encoding frames after recovery

**Test Criteria**:

- ✅ Sink continues operation after encoding error
- ✅ Error is logged to stderr
- ✅ Sink skips failed frame and continues
- ✅ Sink resumes frame encoding after recovery

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`

---

### EH-003: Network Error (TCP Send Failure)

**Condition**: Network output reports send failure (TCP write fails, client disconnect).

**Expected Behavior**:

- Sink logs error: `"Network errors: N"`
- Sink increments `stats.network_errors`
- If `EAGAIN`/`EWOULDBLOCK`: Drop frame, emit backpressure event, continue
- If client disconnect: Tear down muxer, wait for new connection
- Sink does not stop (allows recovery)
- Sink continues streaming after recovery

**Test Criteria**:

- ✅ Sink continues operation after network error
- ✅ Error is logged to stderr
- ✅ Sink retries send after backoff
- ✅ Sink resumes streaming after recovery

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`

---

### EH-004: Buffer Empty

**Condition**: Buffer is empty when timing loop checks (peek returns null).

**Expected Behavior**:

- Sink increments `buffer_empty_count_`
- Sink applies underflow policy (frame freeze, black frame, or skip)
- Sink waits 10ms (`kSinkWaitUs`)
- Sink retries peek on next loop iteration
- Sink does not block or crash
- Sink continues consuming frames when buffer has frames

**Test Criteria**:

- ✅ Sink waits when buffer is empty
- ✅ Sink retries pop after wait
- ✅ Sink does not block or crash
- ✅ Sink resumes frame consumption when buffer has frames

**Test Files**: `tests/test_sink.cpp` (EmptyBufferHandling)

---

### EH-005: Late Frames (Behind Schedule)

**Condition**: Sink is behind schedule (frame PTS is overdue compared to MasterClock time).

**Expected Behavior**:

- Sink detects late frames in timing loop (frame PTS < master_time_us - threshold)
- Sink drops late frames (more than `kLateThresholdUs` late)
- Sink increments `frames_dropped_` and `late_frames_` counters
- Sink continues to next frame (attempts to catch up)
- Sink maintains real-time output (no buffering)

**Test Criteria**:

- ✅ Sink detects late frames correctly
- ✅ Sink drops late frames when behind schedule
- ✅ Sink tracks dropped frames in statistics
- ✅ Sink maintains real-time output pacing

**Test Files**: `tests/test_sink.cpp` (BufferOverrunHandling), `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`

---

### EH-006: Worker Thread Stop

**Condition**: `stop()` is called while worker thread is running.

**Expected Behavior**:

- Sink sets `stop_requested_ = true`
- Worker thread exits timing loop
- Sink waits for worker thread to exit (join thread)
- Sink closes muxer, encoder, and socket
- Sink exits gracefully (may drop frames in buffer)

**Test Criteria**:

- ✅ Worker thread stops when `stop()` is called
- ✅ Sink waits for thread exit (no hanging threads)
- ✅ All resources are cleaned up (muxer, encoder, socket)
- ✅ Sink exits within 100ms of `stop()` call

**Test Files**: `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`

---

## Test Coverage Requirements

All functional expectations (FE-001 through FE-023) must have corresponding test coverage.

### Test File Mapping

| Functional Expectation | Test File                                    | Test Case Name                    |
| ---------------------- | -------------------------------------------- | ---------------------------------- |
| FE-001                 | `tests/test_sink.cpp`                        | Construction, StartStop, CannotStartTwice, StopIdempotent, DestructorStopsSink |
| FE-001                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_001_SinkLifecycle, FE_001_DestructorStopsSink |
| FE-002                 | `tests/test_sink.cpp`                        | FrameOrder, FramePTSMonotonicity   |
| FE-002                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_002_PullsFramesInOrder, FE_002_FramePTSMonotonicity |
| FE-003                 | `tests/test_sink.cpp`                        | MasterClockAlignment               |
| FE-003                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_003_ObeysMasterClock, FE_003_DropsLateFrames |
| FE-004                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_004_EncodesValidH264 |
| FE-005                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_005_ErrorDetectionAndClassification |
| FE-006                 | `tests/test_sink.cpp`                        | EmptyBufferHandling                |
| FE-006                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_006_HandlesEmptyBuffer |
| FE-007                 | `tests/test_sink.cpp`                        | BufferOverrunHandling              |
| FE-007                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_007_HandlesBufferOverrun |
| FE-008                 | `tests/test_sink.cpp`                        | StatisticsAccuracy, BufferOverrunHandling, EmptyBufferHandling |
| FE-008                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_008_ProperlyReportsStats |
| FE-009                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_009_NonblockingWrite |
| FE-010                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_010_QueueOverflow |
| FE-011                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_011_ClientDisconnect |
| FE-012                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_012_ErrorToSinkStatusMapping |
| FE-013                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_013_EncodesRealDecodedFrames |
| FE-014                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_014_GeneratesIncreasingPTSInMpegTSOutput |
| FE-015                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_015_OutputIsReadableByFFprobe |
| FE-016                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_016_FrameTimingIsPreservedThroughEncodingAndMuxing |
| FE-017                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_017_RealTimeOutputTimingStability |
| FE-018                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_018_PTS_DTS_Integrity |
| FE-019                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_019_PCR_Cadence |
| FE-020                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_020_FaultModeBehaviorAndLatching |
| FE-021                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_021_EncoderStallRecovery |
| FE-022                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_022_QueueInvariants |
| FE-023                 | `tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp` | FE_023_TSPacketAlignmentPreserved |

### Coverage Requirements

- ✅ **Unit Tests**: All FE rules must have unit test coverage
- ✅ **Integration Tests**: FE rules involving multiple components must have integration test coverage
- ✅ **Contract Tests**: All FE rules must be verified in contract test suite
- ✅ **Performance Tests**: All PE rules must have performance test coverage

### Test Execution

All tests must:

- Run in < 10 seconds (unit tests) or < 60 seconds (integration tests)
- Produce deterministic results (no flaky tests)
- Provide actionable diagnostics on failure
- Pass in both stub mode and real encode mode (where applicable)
- Verify all frames consumed are decoded (YUV420 format, not encoded packets)
- Verify MPEG-TS stream is playable in VLC (for FE-005)

---

## CI Enforcement

The following rules are enforced in continuous integration:

### Pre-Merge Requirements

1. ✅ All unit tests pass (`tests/test_sink.cpp`)
2. ✅ All contract tests pass (`tests/contracts/MpegTSPlayoutSink/MpegTSPlayoutSinkContractTests.cpp`)
3. ✅ Code coverage ≥ 90% for MpegTSPlayoutSink domain
4. ✅ No memory leaks (valgrind or AddressSanitizer)
5. ✅ No undefined behavior (UB sanitizer)

### Performance Gates

1. ✅ Encoding throughput ≥ `target_fps × 0.95` (5% tolerance)
2. ✅ Frame encoding latency < 33ms (p95) for 1080p30
3. ✅ Memory usage < 200 MB per channel
4. ✅ CPU usage < 40% per channel (1080p30)
5. ✅ Network output rate ≥ `target_fps × 0.95` (5% tolerance)

### Quality Gates

1. ✅ No compiler warnings (treat warnings as errors)
2. ✅ Static analysis passes (clang-tidy, cppcheck)
3. ✅ Documentation is up-to-date (domain doc matches implementation)
4. ✅ All frames verified as decoded (YUV420 format, not encoded packets)
5. ✅ MPEG-TS stream verified as playable in VLC (for FE-015)

---

## See Also

- [MPEG-TS Playout Sink Domain](../domain/MpegTSPlayoutSinkDomain.md) — Domain model and architecture
- [Playout Engine Contract](PlayoutEngineContract.md) — Overall playout engine contract
- [Video File Producer Contract](VideoFileProducerDomainContract.md) — Frame production contract
- [Architecture Overview](../architecture/ArchitectureOverview.md) — System-wide architecture

---

## Design Notes (2025)

### Architectural Pattern

This contract document establishes the **Sink Pattern** for RetroVue Air playout:

**Core Rule**: A Sink in RetroVue consumes *decoded frames* from FrameRingBuffer and outputs them in a specific format (MPEG-TS, file, display, etc.). A Sink does *not* decode frames; frames are already decoded when consumed.

### Key Design Decisions

1. **Unified Sink Model**: MpegTSPlayoutSink performs encoding, muxing, and streaming internally. There is no separate encoder stage in the pipeline. Instead, MpegTSPlayoutSink *contains* the encoding subsystem internally.

2. **Decoded Frames Only**: FrameRingBuffer explicitly contains only decoded frames. Sinks consume decoded frames and transform them into output format (MPEG-TS, file, display).

3. **Simplified Pipeline**: The pipeline is now simpler and more object-oriented:
   - Input: decoded Frame objects from FrameRingBuffer
   - MpegTSPlayoutSink: consumes decoded frames + encodes internally → outputs MPEG-TS stream
   - FrameRingBuffer: stores decoded frames
   - Producer: generates decoded frames (never encoded packets)

4. **Interface Inheritance**: All sinks inherit from `IPlayoutSink`, enabling interchangeable sink implementations and hot-swapping without changing core architecture.

5. **MasterClock Timing**: Sink owns timing loop that continuously queries MasterClock (`master_clock_->now_utc_us()`) and compares with frame PTS to determine when to output. Sink sleeps when ahead of schedule and drops frames when behind schedule. MasterClock never pushes ticks (sink pulls time from MasterClock).

### Benefits

- **Cleaner Architecture**: Clear separation of concerns with Sinks handling all encoding/output internally
- **Simplified Pipeline**: No separate encode stage simplifies the system
- **Consistent Pattern**: All sinks follow the same pattern (consume decoded frames, output in specific format)
- **Better Encapsulation**: Encoding operations are internal implementation details
- **Interchangeable Sinks**: Base interface enables hot-swapping of sink implementations
- **Real-Time Guarantees**: MasterClock timing ensures broadcast timing accuracy

