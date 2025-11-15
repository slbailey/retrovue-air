// Repository: Retrovue-playout
// Component: Stub Muxer for Testing
// Purpose: Test double for TSMuxer that tracks calls without real muxing.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_MUXER_H_
#define RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_MUXER_H_

#include "retrovue/playout_sinks/mpegts/TSMuxer.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace retrovue::tests::fixtures::mpegts_sink {

// StubMuxer replaces real FFmpeg muxer in tests.
// Tracks initialization and muxing calls without performing real muxing.
class StubMuxer : public retrovue::playout_sinks::mpegts::TSMuxer {
 public:
  StubMuxer() : init_count_(0), write_count_(0), should_fail_init_(false) {
    is_initialized_ = false;
    output_fd_ = -1;
  }

  // Override Initialize to track calls
  bool Initialize(
      const retrovue::playout_sinks::mpegts::MuxerConfig& config,
      int output_fd) override {
    init_count_.fetch_add(1, std::memory_order_acq_rel);
    if (should_fail_init_) {
      return false;
    }
    // Stub: mark as initialized without real FFmpeg
    is_initialized_ = true;
    output_fd_ = output_fd;
    return true;
  }

  // Override MuxPacket to track calls
  bool MuxPacket(const std::vector<uint8_t>& packet_data, int64_t pts_us) override {
    write_count_.fetch_add(1, std::memory_order_acq_rel);
    last_packet_pts_ = pts_us;
    return TSMuxer::MuxPacket(packet_data, pts_us);
  }

  // Test inspection methods
  uint64_t GetInitCount() const {
    return init_count_.load(std::memory_order_acquire);
  }

  uint64_t GetWriteCount() const {
    return write_count_.load(std::memory_order_acquire);
  }

  int64_t GetLastPacketPts() const {
    return last_packet_pts_.load(std::memory_order_acquire);
  }

  // Test control methods
  void SetShouldFailInit(bool should_fail) {
    should_fail_init_ = should_fail;
  }

  void Reset() {
    init_count_.store(0, std::memory_order_release);
    write_count_.store(0, std::memory_order_release);
    last_packet_pts_.store(0, std::memory_order_release);
    should_fail_init_ = false;
  }

 private:
  std::atomic<uint64_t> init_count_;
  std::atomic<uint64_t> write_count_;
  std::atomic<int64_t> last_packet_pts_;
  bool should_fail_init_;
};

}  // namespace retrovue::tests::fixtures::mpegts_sink

#endif  // RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_STUB_MUXER_H_

