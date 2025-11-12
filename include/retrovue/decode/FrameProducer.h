// Repository: Retrovue-playout
// Component: Frame Producer
// Purpose: Decodes media assets and produces frames for the ring buffer.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_DECODE_FRAME_PRODUCER_H_
#define RETROVUE_DECODE_FRAME_PRODUCER_H_

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "retrovue/buffer/FrameRingBuffer.h"

namespace retrovue::timing {
class MasterClock;
}

namespace retrovue::decode {

namespace timing = ::retrovue::timing;

// ProducerConfig holds configuration for frame production.
struct ProducerConfig {
  std::string asset_uri;       // URI or path to media asset
  int target_width;            // Target frame width (e.g., 1920)
  int target_height;           // Target frame height (e.g., 1080)
  double target_fps;           // Target frames per second (e.g., 30.0)
  bool stub_mode;              // If true, generate fake frames instead of decoding
  bool hw_accel_enabled;       // Enable hardware acceleration (passed to FFmpegDecoder)
  int max_decode_threads;      // Maximum decoder threads (0 = auto)
  
  ProducerConfig()
      : target_width(1920),
        target_height(1080),
        target_fps(30.0),
        stub_mode(false),  // Phase 3: default to real decode
        hw_accel_enabled(false),
        max_decode_threads(0) {}
};

// Forward declaration
class FFmpegDecoder;

// FrameProducer runs a decode loop that fills a frame ring buffer.
//
// Phase 3 Implementation:
// - Real decode using FFmpegDecoder (libavformat/libavcodec)
// - Stub mode available for testing (set config.stub_mode = true)
// - Automatic decoder initialization and error recovery
//
// Thread Model:
// - Producer runs in its own thread
// - Continuously produces frames until stopped
// - Backs off when ring buffer is full
//
// Lifecycle:
// 1. Construct with config and ring buffer reference
// 2. Call Start() to begin production
// 3. Call Stop() to gracefully shutdown
// 4. Destructor ensures thread is joined
class FrameProducer {
 public:
  // Constructs a producer with the given configuration and output buffer.
  FrameProducer(const ProducerConfig& config, 
                buffer::FrameRingBuffer& output_buffer,
                std::shared_ptr<timing::MasterClock> clock = nullptr);
  
  ~FrameProducer();

  // Disable copy and move
  FrameProducer(const FrameProducer&) = delete;
  FrameProducer& operator=(const FrameProducer&) = delete;

  // Starts the decode thread.
  // Returns true if started successfully, false if already running.
  bool Start();

  // Stops the decode thread gracefully.
  // Blocks until the thread exits.
  void Stop();

  // Initiates a graceful teardown with bounded drain timeout.
  void RequestTeardown(std::chrono::milliseconds drain_timeout);

  // Forces the producer to stop immediately (used when teardown times out).
  void ForceStop();

  // Returns true if the producer is currently running.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Returns the total number of frames produced.
  uint64_t GetFramesProduced() const { 
    return frames_produced_.load(std::memory_order_acquire);
  }

  // Returns the number of times the buffer was full (frame drops).
  uint64_t GetBufferFullCount() const {
    return buffer_full_count_.load(std::memory_order_acquire);
  }

 private:
  // Main decode loop (runs in producer thread).
  void ProduceLoop();

  // Stub implementation: generates fake frames.
  void ProduceStubFrame();

  // Real decode implementation using FFmpegDecoder.
  void ProduceRealFrame();

  ProducerConfig config_;
  buffer::FrameRingBuffer& output_buffer_;
  
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::atomic<uint64_t> frames_produced_;
  std::atomic<uint64_t> buffer_full_count_;
  
  std::unique_ptr<std::thread> producer_thread_;
  std::unique_ptr<FFmpegDecoder> decoder_;
  std::shared_ptr<timing::MasterClock> master_clock_;

  std::atomic<bool> teardown_requested_;
  std::chrono::steady_clock::time_point teardown_deadline_;
  std::chrono::milliseconds drain_timeout_;
  
  // State for stub frame generation
  int64_t stub_pts_counter_;
  int64_t frame_interval_us_;
  int64_t next_stub_deadline_utc_;
};

}  // namespace retrovue::decode

#endif  // RETROVUE_DECODE_FRAME_PRODUCER_H_

