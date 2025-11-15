// Repository: Retrovue-playout
// Component: MPEG-TS Muxer
// Purpose: Wraps FFmpeg MPEG-TS muxing for MpegTSPlayoutSink.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_TS_MUXER_H_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_TS_MUXER_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace retrovue::playout_sinks::mpegts {

// Forward declaration
struct MuxerConfig;

// TSMuxer wraps FFmpeg MPEG-TS muxing.
// Packages H.264 packets into MPEG-TS transport stream format.
class TSMuxer {
 public:
  TSMuxer();
  virtual ~TSMuxer();

  // Initialize muxer with configuration
  // output_fd: File descriptor for output (TCP socket)
  // Returns true on success, false on failure
  virtual bool Initialize(const MuxerConfig& config, int output_fd);

  // Cleanup muxer resources
  virtual void Cleanup();

  // Mux an encoded H.264 packet into MPEG-TS
  // packet_data: H.264 encoded packet data
  // pts_us: Presentation timestamp in microseconds
  // Returns true on success, false on failure
  virtual bool MuxPacket(const std::vector<uint8_t>& packet_data, int64_t pts_us);

  // Write any buffered MPEG-TS data
  // Returns true on success, false on failure
  virtual bool Flush();

  // Check if muxer is initialized
  bool IsInitialized() const { return is_initialized_; }

 protected:
  bool is_initialized_ = false;
  int output_fd_ = -1;

 private:
  
  // TODO: Add FFmpeg muxer state (AVFormatContext, etc.)
  // struct MuxerState;
  // std::unique_ptr<MuxerState> muxer_state_;
};

// Muxer configuration
struct MuxerConfig {
  bool enable_audio = false;  // Enable silent AAC audio track
  bool stub_mode = false;      // Use stub muxing (no real FFmpeg)
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_TS_MUXER_H_

