// Repository: Retrovue-playout
// Component: MPEG-TS Playout Sink Configuration
// Purpose: Configuration structure for MpegTSPlayoutSink.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_CONFIG_HPP_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_CONFIG_HPP_

#include <string>

namespace retrovue::playout_sinks::mpegts {

// Underflow policy when buffer is empty
enum class UnderflowPolicy {
  FRAME_FREEZE,  // Repeat last frame (default)
  BLACK_FRAME,   // Output black frame
  SKIP           // Skip output
};

// Configuration for MpegTSPlayoutSink
// POD struct - immutable after construction
struct MpegTSPlayoutSinkConfig {
  int port = 9000;                    // TCP server port
  std::string bind_host = "127.0.0.1"; // TCP bind address (default: localhost)
  double target_fps = 30.0;           // Target frame rate
  int bitrate = 5000000;              // Encoding bitrate (5 Mbps)
  int gop_size = 30;                  // GOP size (1 second at 30fps)
  bool stub_mode = false;             // Use stub mode (no real encoding)
  UnderflowPolicy underflow_policy = UnderflowPolicy::FRAME_FREEZE;
  bool enable_audio = false;          // Enable silent AAC audio
  size_t max_output_queue_packets = 100;  // Max packets in output queue before dropping
  size_t output_queue_high_water_mark = 80;  // High water mark: encode new frames only if queue below this
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_PLAYOUT_SINK_CONFIG_HPP_

