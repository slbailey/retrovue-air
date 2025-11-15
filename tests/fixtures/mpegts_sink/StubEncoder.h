// Repository: Retrovue-playout
// Component: Stub Encoder for Testing
// Purpose: Test double for MpegTSEncoder that tracks calls without real encoding.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_H_
#define RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_H_

#include "retrovue/playout_sinks/mpegts/MpegTSEncoder.h"
#include "retrovue/buffer/FrameRingBuffer.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace retrovue::tests::fixtures::mpegts_sink {

// StubEncoder replaces real FFmpeg encoder in tests.
// Tracks initialization and encoding calls without performing real encoding.
class StubEncoder : public retrovue::playout_sinks::mpegts::MpegTSEncoder {
 public:
  StubEncoder() : init_count_(0), encode_count_(0), should_fail_init_(false) {
    is_initialized_ = false;
  }

  // Override Initialize to track calls
  bool Initialize(const retrovue::playout_sinks::mpegts::EncoderConfig& config) override {
    init_count_.fetch_add(1, std::memory_order_acq_rel);
    if (should_fail_init_) {
      return false;
    }
    // Stub: mark as initialized without real FFmpeg
    is_initialized_ = true;
    return true;
  }

  // Override EncodeFrame to track calls
  std::vector<uint8_t> EncodeFrame(
      const retrovue::buffer::Frame& frame,
      int64_t pts_us) override {
    encode_count_.fetch_add(1, std::memory_order_acq_rel);
    last_frame_pts_ = pts_us;
    // Return dummy encoded data
    return std::vector<uint8_t>(100, 0x42);
  }

  // Test inspection methods
  uint64_t GetInitCount() const {
    return init_count_.load(std::memory_order_acquire);
  }

  uint64_t GetEncodeCount() const {
    return encode_count_.load(std::memory_order_acquire);
  }

  int64_t GetLastFramePts() const {
    return last_frame_pts_.load(std::memory_order_acquire);
  }

  // Test control methods
  void SetShouldFailInit(bool should_fail) {
    should_fail_init_ = should_fail;
  }

  void Reset() {
    init_count_.store(0, std::memory_order_release);
    encode_count_.store(0, std::memory_order_release);
    last_frame_pts_.store(0, std::memory_order_release);
    should_fail_init_ = false;
  }

 private:
  std::atomic<uint64_t> init_count_;
  std::atomic<uint64_t> encode_count_;
  std::atomic<int64_t> last_frame_pts_;
  bool should_fail_init_;
};

}  // namespace retrovue::tests::fixtures::mpegts_sink

#endif  // RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_H_

