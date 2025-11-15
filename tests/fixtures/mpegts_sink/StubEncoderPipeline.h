// Repository: Retrovue-playout
// Component: Stub Encoder Pipeline for Testing
// Purpose: Test double for EncoderPipeline that tracks calls without real encoding.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_PIPELINE_H_
#define RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_PIPELINE_H_

#include "retrovue/playout_sinks/mpegts/EncoderPipeline.hpp"
#include "retrovue/buffer/FrameRingBuffer.h"
#include <atomic>
#include <cstdint>
#include <vector>
#include <memory>

namespace retrovue::tests::fixtures::mpegts_sink {

// StubEncoderPipeline replaces real EncoderPipeline in tests.
// Tracks initialization and encoding calls without performing real encoding.
class StubEncoderPipeline : public retrovue::playout_sinks::mpegts::EncoderPipeline {
 public:
  explicit StubEncoderPipeline(const retrovue::playout_sinks::mpegts::MpegTSPlayoutSinkConfig& config)
      : EncoderPipeline(config),
        init_count_(0),
        encode_count_(0),
        should_fail_init_(false),
        should_fail_encode_(false),
        initialized_(false) {
  }

  // Override open to track calls
  bool open(const retrovue::playout_sinks::mpegts::MpegTSPlayoutSinkConfig& config) override {
    init_count_.fetch_add(1, std::memory_order_acq_rel);
    if (should_fail_init_) {
      return false;
    }
    initialized_ = true;
    return true;
  }

  bool open(const retrovue::playout_sinks::mpegts::MpegTSPlayoutSinkConfig& config,
            std::function<bool(const uint8_t* data, size_t size)> write_callback) override {
    init_count_.fetch_add(1, std::memory_order_acq_rel);
    if (should_fail_init_) {
      return false;
    }
    initialized_ = true;
    return true;
  }

  // Override encodeFrame to track calls
  bool encodeFrame(const retrovue::buffer::Frame& frame, int64_t pts90k) override {
    encode_count_.fetch_add(1, std::memory_order_acq_rel);
    last_frame_pts90k_.store(pts90k, std::memory_order_release);
    if (should_fail_encode_) {
      return false;
    }
    // In stub mode, we don't actually encode, just track the call
    // Return true to indicate success
    return true;
  }

  // Override close
  void close() override {
    initialized_ = false;
  }

  // Override IsInitialized (note: base class method is const)
  bool IsInitialized() const override {
    return initialized_;
  }

  // Test inspection methods
  uint64_t GetInitCount() const {
    return init_count_.load(std::memory_order_acquire);
  }

  uint64_t GetEncodeCount() const {
    return encode_count_.load(std::memory_order_acquire);
  }

  int64_t GetLastFramePts90k() const {
    return last_frame_pts90k_.load(std::memory_order_acquire);
  }

  // Test control methods
  void SetShouldFailInit(bool should_fail) {
    should_fail_init_ = should_fail;
  }

  void SetShouldFailEncode(bool should_fail) {
    should_fail_encode_ = should_fail;
  }

  void Reset() {
    init_count_.store(0, std::memory_order_release);
    encode_count_.store(0, std::memory_order_release);
    last_frame_pts90k_.store(0, std::memory_order_release);
    should_fail_init_ = false;
    should_fail_encode_ = false;
    initialized_ = false;
  }

 private:
  std::atomic<uint64_t> init_count_;
  std::atomic<uint64_t> encode_count_;
  std::atomic<int64_t> last_frame_pts90k_;
  bool should_fail_init_;
  bool should_fail_encode_;
  bool initialized_;
};

}  // namespace retrovue::tests::fixtures::mpegts_sink

#endif  // RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_ENCODER_PIPELINE_H_

