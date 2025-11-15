// Repository: Retrovue-playout
// Component: Frame Factory for Testing
// Purpose: Generate synthetic decoded frames for testing.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_FRAME_FACTORY_H_
#define RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_FRAME_FACTORY_H_

#include "retrovue/buffer/FrameRingBuffer.h"
#include <cstdint>
#include <string>

namespace retrovue::tests::fixtures::mpegts_sink {

// FrameFactory generates synthetic decoded frames for testing.
// Creates YUV420 frames with specified dimensions and PTS.
class FrameFactory {
 public:
  // Create a synthetic YUV420 frame
  // pts_us: Presentation timestamp in microseconds
  // width: Frame width (default 1920)
  // height: Frame height (default 1080)
  // Returns a Frame with synthetic YUV420 data
  static retrovue::buffer::Frame CreateFrame(
      int64_t pts_us,
      int width = 1920,
      int height = 1080) {
    retrovue::buffer::Frame frame;
    frame.metadata.pts = pts_us;
    frame.metadata.dts = pts_us;
    frame.metadata.duration = 1.0 / 30.0;  // 30 fps default
    frame.metadata.asset_uri = "test://synthetic_frame";
    frame.width = width;
    frame.height = height;

    // Generate synthetic YUV420 data
    // Y plane: width * height bytes
    // U plane: (width/2) * (height/2) bytes
    // V plane: (width/2) * (height/2) bytes
    size_t y_size = width * height;
    size_t uv_size = (width / 2) * (height / 2);
    size_t total_size = y_size + uv_size + uv_size;

    frame.data.resize(total_size);

    // Fill Y plane with pattern (simple gradient)
    for (size_t i = 0; i < y_size; ++i) {
      frame.data[i] = static_cast<uint8_t>((i * 255) / y_size);
    }

    // Fill U and V planes with constant values
    uint8_t u_value = 128;
    uint8_t v_value = 128;
    for (size_t i = 0; i < uv_size; ++i) {
      frame.data[y_size + i] = u_value;
      frame.data[y_size + uv_size + i] = v_value;
    }

    return frame;
  }

  // Create a sequence of frames with increasing PTS
  // start_pts_us: PTS of first frame
  // frame_interval_us: Time between frames (e.g., 33333 for 30fps)
  // count: Number of frames to create
  // Returns vector of frames
  static std::vector<retrovue::buffer::Frame> CreateFrameSequence(
      int64_t start_pts_us,
      int64_t frame_interval_us,
      size_t count) {
    std::vector<retrovue::buffer::Frame> frames;
    frames.reserve(count);

    for (size_t i = 0; i < count; ++i) {
      int64_t pts_us = start_pts_us + (i * frame_interval_us);
      frames.push_back(CreateFrame(pts_us));
    }

    return frames;
  }
};

}  // namespace retrovue::tests::fixtures::mpegts_sink

#endif  // RETROVUE_TESTS_FIXTURES_MPEGTS_SINK_FRAME_FACTORY_H_





