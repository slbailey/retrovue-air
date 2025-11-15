// Repository: Retrovue-playout
// Component: MPEG-TS Playout Sink
// Purpose: Encodes decoded frames to H.264, muxes to MPEG-TS, streams over TCP.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_H_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_H_

#include "retrovue/playout_sinks/IPlayoutSink.h"
#include "retrovue/playout_sinks/mpegts/SinkConfig.h"
#include "retrovue/playout_sinks/mpegts/SinkStats.h"
#include "retrovue/playout_sinks/mpegts/MpegTSEncoder.h"
#include "retrovue/playout_sinks/mpegts/TSMuxer.h"
#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/timing/MasterClock.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <cstdint>
#include <vector>

namespace retrovue::playout_sinks::mpegts {

// MpegTSPlayoutSink consumes decoded frames from FrameRingBuffer,
// encodes them to H.264, muxes to MPEG-TS, and streams over TCP socket.
// The sink owns its timing loop and continuously queries MasterClock
// to determine when to output frames.
//
// Critical: MasterClock never pushes ticks or callbacks.
// The sink calls master_clock_->now_utc_us() whenever it needs the current time.
class MpegTSPlayoutSink : public IPlayoutSink {
 public:
  MpegTSPlayoutSink(
      const SinkConfig& config,
      buffer::FrameRingBuffer& input_buffer,
      std::shared_ptr<timing::MasterClock> master_clock
  );

  // Test constructor: allows dependency injection of encoder and muxer
  // For testing only - allows injecting stub encoder/muxer
  MpegTSPlayoutSink(
      const SinkConfig& config,
      buffer::FrameRingBuffer& input_buffer,
      std::shared_ptr<timing::MasterClock> master_clock,
      std::unique_ptr<MpegTSEncoder> encoder,
      std::unique_ptr<TSMuxer> muxer
  );
  
  ~MpegTSPlayoutSink() override;

  // IPlayoutSink interface
  bool start() override;
  void stop() override;
  bool isRunning() const override;

  // Statistics accessors
  SinkStats getStats() const;

 private:
  // Worker thread that owns the timing loop
  // Continuously queries MasterClock and compares with frame PTS
  void WorkerLoop();

  // Process a single frame (encode, mux, send)
  // frame: Decoded frame from buffer
  // master_time_us: Current MasterClock time
  void ProcessFrame(const buffer::Frame& frame, int64_t master_time_us);

  // Handle buffer underflow (empty buffer)
  // master_time_us: Current MasterClock time
  void HandleBufferUnderflow(int64_t master_time_us);

  // Handle buffer overflow (drop late frames)
  // master_time_us: Current MasterClock time
  void HandleBufferOverflow(int64_t master_time_us);

  // Initialize TCP socket (listen, accept)
  // Returns true on success, false on failure
  bool InitializeSocket();
  void CleanupSocket();

  // Accept thread function (handles client connections)
  void AcceptThread();

  // Send data to TCP socket (non-blocking)
  // Returns true on success, false on failure (including EAGAIN)
  bool SendToSocket(const uint8_t* data, size_t size);

  // Map media PTS to station time
  // Called on first frame to establish pts_zero_utc_us
  void InitializePTSMapping(int64_t first_frame_pts);

  // Calculate target station time for a frame
  // frame_pts: Media PTS in microseconds
  // Returns target station time in microseconds
  int64_t CalculateTargetTime(int64_t frame_pts) const;

  // Configuration (immutable after construction)
  SinkConfig config_;
  buffer::FrameRingBuffer& buffer_;
  std::shared_ptr<timing::MasterClock> master_clock_;

  // Threading
  std::atomic<bool> is_running_{false};
  std::atomic<bool> stop_requested_{false};
  std::thread worker_thread_;
  std::mutex state_mutex_;

  // TCP socket
  int listen_fd_{-1};
  int client_fd_{-1};
  std::atomic<bool> client_connected_{false};
  std::thread accept_thread_;

  // Encoder and Muxer
  std::unique_ptr<MpegTSEncoder> encoder_;
  std::unique_ptr<TSMuxer> muxer_;

  // PTS mapping state
  // pts_zero_utc_us is set on first frame: master_clock_->now_utc_us() - frame.pts
  // For subsequent frames: target_utc_us = pts_zero_utc_us + frame.pts
  bool pts_mapping_initialized_{false};
  int64_t pts_zero_utc_us_{0};

  // Last encoded frame (for frame freeze underflow policy)
  std::vector<uint8_t> last_encoded_frame_;

  // Statistics (atomic for thread safety)
  std::atomic<uint64_t> frames_sent_{0};
  std::atomic<uint64_t> frames_dropped_{0};
  std::atomic<uint64_t> late_frames_{0};
  std::atomic<uint64_t> encoding_errors_{0};
  std::atomic<uint64_t> network_errors_{0};
  std::atomic<uint64_t> buffer_underruns_{0};
  std::atomic<uint64_t> late_frame_drops_{0};
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_H_

