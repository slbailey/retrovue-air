// Repository: Retrovue-playout
// Component: MPEG-TS Encoder
// Purpose: Wraps FFmpeg H.264 encoding for MpegTSPlayoutSink.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_ENCODER_H_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_ENCODER_H_

#include "retrovue/buffer/FrameRingBuffer.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace retrovue::playout_sinks::mpegts {

// Forward declaration
struct EncoderConfig;

// MpegTSEncoder wraps FFmpeg H.264 encoding.
// Encodes decoded YUV420 frames into H.264 NAL units.
class MpegTSEncoder {
 public:
  MpegTSEncoder();
  virtual ~MpegTSEncoder();

  // Initialize encoder with configuration
  // Returns true on success, false on failure
  virtual bool Initialize(const EncoderConfig& config);

  // Cleanup encoder resources
  virtual void Cleanup();

  // Encode a decoded frame to H.264
  // frame: Input decoded frame (YUV420 format)
  // pts_us: Presentation timestamp in microseconds
  // Returns encoded packet data (empty on error)
  virtual std::vector<uint8_t> EncodeFrame(const buffer::Frame& frame, int64_t pts_us);

  // Check if encoder is initialized
  bool IsInitialized() const { return is_initialized_; }

 protected:
  bool is_initialized_ = false;

 private:
  
  // TODO: Add FFmpeg encoder state (AVCodecContext, etc.)
  // struct EncoderState;
  // std::unique_ptr<EncoderState> encoder_state_;
};

// Encoder configuration
struct EncoderConfig {
  int width = 1920;
  int height = 1080;
  int bitrate = 5000000;      // 5 Mbps
  int gop_size = 30;         // 1 second at 30fps
  double target_fps = 30.0;
  bool stub_mode = false;    // Use stub encoding (no real FFmpeg)
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_MPEGTS_ENCODER_H_

