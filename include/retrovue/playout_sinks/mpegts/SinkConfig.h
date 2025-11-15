// Repository: Retrovue-playout
// Component: MPEG-TS Playout Sink Configuration
// Purpose: Configuration structure for MpegTSPlayoutSink.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_CONFIG_H_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_CONFIG_H_

namespace retrovue::playout_sinks::mpegts {

// Underflow policy when buffer is empty
enum class UnderflowPolicy {
  FRAME_FREEZE,  // Repeat last frame (default)
  BLACK_FRAME,   // Output black frame
  SKIP           // Skip output
};

// Configuration for MpegTSPlayoutSink
// POD struct - immutable after construction
struct SinkConfig {
  int port = 9000;                    // TCP server port
  double target_fps = 30.0;           // Target frame rate
  int bitrate = 5000000;              // Encoding bitrate (5 Mbps)
  int gop_size = 30;                  // GOP size (1 second at 30fps)
  bool stub_mode = false;             // Use stub mode (no real encoding)
  UnderflowPolicy underflow_policy = UnderflowPolicy::FRAME_FREEZE;
  bool enable_audio = false;          // Enable silent AAC audio
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_CONFIG_H_





