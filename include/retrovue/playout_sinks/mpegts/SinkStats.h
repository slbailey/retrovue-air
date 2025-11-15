// Repository: Retrovue-playout
// Component: MPEG-TS Playout Sink Statistics
// Purpose: Statistics structure for MpegTSPlayoutSink.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_STATS_H_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_STATS_H_

#include <cstdint>

namespace retrovue::playout_sinks::mpegts {

// Statistics for MpegTSPlayoutSink
// POD struct - thread-safe atomic counters
struct SinkStats {
  uint64_t frames_sent = 0;           // Total frames successfully encoded and sent
  uint64_t frames_dropped = 0;        // Frames dropped due to lateness
  uint64_t late_frames = 0;           // Frames that were late (within tolerance)
  uint64_t encoding_errors = 0;      // Encoder errors
  uint64_t network_errors = 0;        // Network send errors
  uint64_t buffer_underruns = 0;      // Buffer empty events
  uint64_t late_frame_drops = 0;     // Frames dropped because too late
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_SINK_STATS_H_





