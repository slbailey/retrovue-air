// Repository: Retrovue-playout
// Component: MPEG-TS Playout Sink
// Purpose: Encodes decoded frames to H.264, muxes to MPEG-TS, streams over TCP.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_HPP_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_HPP_

#include "retrovue/playout_sinks/IPlayoutSink.h"
#include "retrovue/playout_sinks/mpegts/MpegTSPlayoutSinkConfig.hpp"
#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/timing/MasterClock.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <deque>
#include <vector>

namespace retrovue::playout_sinks::mpegts {

// Forward declarations
class PTSController;
class EncoderPipeline;

// Packet type for encoded packets
enum class PacketType {
  AUDIO,
  VIDEO
};

// Encoded packet structure for output queue
struct EncodedPacket {
  PacketType type;
  std::vector<uint8_t> data;
  int64_t pts90k;
  
  EncodedPacket() : type(PacketType::VIDEO), pts90k(0) {}
  EncodedPacket(PacketType type, std::vector<uint8_t> data, int64_t pts90k) 
    : type(type), data(std::move(data)), pts90k(pts90k) {}
};

// Internal state machine states
enum class InternalState {
  Idle,              // Initial state, not started
  WaitingForClient,  // Waiting for TCP client to connect
  Running,           // Active playout, encoding and streaming
  Stopped,           // Gracefully stopped
  Error              // Error state, requires recovery
};

// MpegTSPlayoutSink consumes decoded frames from FrameRingBuffer,
// encodes them to H.264, muxes to MPEG-TS, and streams over TCP socket.
// The sink owns its timing loop and continuously queries MasterClock
// to determine when to output frames.
//
// Critical: MasterClock never pushes ticks or callbacks.
// The sink calls master_clock_->now_utc_us() whenever it needs the current time.
class MpegTSPlayoutSink : public IPlayoutSink {
 public:
  // Constructs sink with frame buffer, master clock, and configuration.
  MpegTSPlayoutSink(
      std::shared_ptr<retrovue::buffer::FrameRingBuffer> frame_buffer,
      std::shared_ptr<retrovue::timing::MasterClock> master_clock,
      const MpegTSPlayoutSinkConfig& config);

  // Test constructor: allows dependency injection of EncoderPipeline
  // For testing only - allows injecting stub encoder pipeline
  MpegTSPlayoutSink(
      std::shared_ptr<::retrovue::buffer::FrameRingBuffer> frame_buffer,
      std::shared_ptr<::retrovue::timing::MasterClock> master_clock,
      const MpegTSPlayoutSinkConfig& config,
      std::unique_ptr<EncoderPipeline> encoder_pipeline);

  ~MpegTSPlayoutSink() override;

  // Disable copy and move
  MpegTSPlayoutSink(const MpegTSPlayoutSink&) = delete;
  MpegTSPlayoutSink& operator=(const MpegTSPlayoutSink&) = delete;
  MpegTSPlayoutSink(MpegTSPlayoutSink&&) = delete;
  MpegTSPlayoutSink& operator=(MpegTSPlayoutSink&&) = delete;

  // IPlayoutSink interface
  bool start() override;
  void stop() override;
  bool isRunning() const override;

  // Returns the current internal state.
  InternalState state() const;

  // Returns the sink name for logging/identification.
  std::string name() const;

  // Statistics accessors (for compatibility with contract tests)
  // Note: This is a compatibility method - the actual implementation uses different architecture
  struct SinkStats {
    uint64_t frames_sent = 0;
    uint64_t frames_dropped = 0;
    uint64_t late_frames = 0;
    uint64_t encoding_errors = 0;
    uint64_t network_errors = 0;
    uint64_t buffer_underruns = 0;
    uint64_t late_frame_drops = 0;
  };
  SinkStats getStats() const;

 private:
  // Worker thread that owns the timing loop.
  // Continuously queries MasterClock and compares with frame PTS.
  // TODO: Implement full timing loop logic
  void workerLoop();

  // Process a single frame (encode, mux, send).
  // frame: Decoded frame from buffer
  // master_time_us: Current MasterClock time
  // pts90k: PTS in 90kHz units
  // frame_number: Frame sequence number for logging
  // drift_us: Timing drift in microseconds
  void processFrame(const retrovue::buffer::Frame& frame, int64_t master_time_us,
                    int64_t pts90k, uint64_t frame_number, int64_t drift_us);

  // Handle buffer underflow (empty buffer).
  // master_time_us: Current MasterClock time
  // TODO: Implement underflow policy (frame freeze/black/skip)
  void handleBufferUnderflow(int64_t master_time_us);

  // Handle buffer overflow (drop late frames).
  // master_time_us: Current MasterClock time
  // TODO: Implement late frame dropping logic
  void handleBufferOverflow(int64_t master_time_us);

  // Initialize TCP socket (listen, accept).
  // Returns true on success, false on failure.
  // TODO: Implement socket initialization
  bool initializeSocket();

  // Cleanup TCP socket resources.
  // TODO: Implement socket cleanup
  void cleanupSocket();

  // Accept thread function (handles client connections).
  // Runs in separate thread to avoid blocking worker loop.
  // TODO: Implement TCP accept loop
  void acceptThread();

  // Send data to TCP socket (non-blocking).
  // Returns true on success, false on failure (including EAGAIN).
  // TODO: Implement non-blocking socket send
  bool sendToSocket(const uint8_t* data, size_t size);
  
  // Try to drain output queue (send pending packets).
  // Returns number of packets successfully sent.
  // Non-blocking: returns immediately if socket would block.
  size_t drainOutputQueue();
  
  // Queue an encoded packet for sending.
  // Returns true if queued, false if queue overflow (packet dropped).
  bool queueEncodedPacket(PacketType type, std::vector<uint8_t> data, int64_t pts90k);
  
  // Handle client disconnect (cleanup and prepare for reconnect).
  void handleClientDisconnect();
  
  // Try to accept a new client connection (non-blocking).
  // Returns true if client connected, false otherwise.
  bool tryAcceptClient();
  
  // Initialize encoder pipeline for new client.
  // Returns true on success, false on failure.
  bool initializeEncoderForClient();

  // Configuration (immutable after construction)
  MpegTSPlayoutSinkConfig config_;
  std::shared_ptr<retrovue::buffer::FrameRingBuffer> frame_buffer_;
  std::shared_ptr<retrovue::timing::MasterClock> master_clock_;

  // State management
  mutable std::mutex state_mutex_;
  InternalState internal_state_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;

  // Threading
  std::thread worker_thread_;
  std::thread accept_thread_;  // Optional: for accepting TCP clients

  // TCP socket
  int listen_fd_;
  int client_fd_;
  std::atomic<bool> client_connected_;

  // Subsystems
  std::unique_ptr<PTSController> pts_controller_;
  std::unique_ptr<EncoderPipeline> encoder_pipeline_;

  // Output queue for encoded packets
  std::deque<EncodedPacket> output_queue_;
  mutable std::mutex output_queue_mutex_;
  std::atomic<uint64_t> packets_dropped_{0};  // Packets dropped due to queue overflow

  // Playout timing state
  // sink_start_time_utc_us is recorded at start() to establish program start time
  // For a frame with pts_usec=X, target emission time is: sink_start_time_utc_us + X
  int64_t sink_start_time_utc_us_{0};
  bool sink_start_time_recorded_{false};

  // Statistics (atomic for thread safety)
  std::atomic<uint64_t> frames_sent_;
  std::atomic<uint64_t> frames_dropped_;
  std::atomic<uint64_t> late_frames_;
  std::atomic<uint64_t> encoding_errors_;
  std::atomic<uint64_t> network_errors_;
  std::atomic<uint64_t> buffer_underruns_;
  std::atomic<uint64_t> late_frame_drops_;
  std::atomic<uint64_t> dropped_packets_{0};  // Packets dropped due to EAGAIN

 public:
  // FE-017: Write all bytes atomically (blocks until complete or error)
  // This ensures TS packets are never split, preserving continuity counters
  // Returns number of bytes written, or -1 on error
  int writeAllBlocking(uint8_t* buf, int buf_size);
};

// C-style callback for FFmpeg AVIO (must be in global scope or extern "C")
extern "C" int writePacketCallback(void* opaque, uint8_t* buf, int buf_size);

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_HPP_

