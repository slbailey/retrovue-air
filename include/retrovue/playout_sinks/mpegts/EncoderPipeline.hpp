// Repository: Retrovue-playout
// Component: Encoder Pipeline
// Purpose: Owns FFmpeg encoder/muxer handles and manages encoding lifecycle.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_ENCODER_PIPELINE_HPP_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_ENCODER_PIPELINE_HPP_

#include "retrovue/playout_sinks/mpegts/MpegTSPlayoutSinkConfig.hpp"

#include <cstdint>
#include <memory>
#include <vector>
#include <functional>
#include <map>

#ifdef RETROVUE_FFMPEG_AVAILABLE
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

namespace retrovue::buffer {
struct Frame;
}  // namespace retrovue::buffer

namespace retrovue::playout_sinks::mpegts {

// EncoderPipeline owns FFmpeg encoder and muxer handles.
// It initializes the encoder in open(), encodes frames via encodeFrame(),
// and closes the muxer on close().
class EncoderPipeline {
 public:
  explicit EncoderPipeline(const MpegTSPlayoutSinkConfig& config);
  virtual ~EncoderPipeline();

  // Disable copy and move
  EncoderPipeline(const EncoderPipeline&) = delete;
  EncoderPipeline& operator=(const EncoderPipeline&) = delete;
  EncoderPipeline(EncoderPipeline&&) = delete;
  EncoderPipeline& operator=(EncoderPipeline&&) = delete;

  // Initialize encoder and muxer.
  // Must be called before encoding frames.
  // Returns true on success, false on failure.
  virtual bool open(const MpegTSPlayoutSinkConfig& config);
  
  // Initialize encoder and muxer with C-style write callback (for nonblocking mode)
  // opaque: Opaque pointer passed to write_callback
  // write_callback: C-style callback for writing encoded packets
  //   Callback signature: int write_callback(void* opaque, uint8_t* buf, int buf_size)
  //   Must always return buf_size (never block, never return < buf_size)
  virtual bool open(const MpegTSPlayoutSinkConfig& config, 
            void* opaque,
            int (*write_callback)(void* opaque, uint8_t* buf, int buf_size));

  // Encode a frame and mux into MPEG-TS.
  // frame: Decoded frame to encode
  // pts90k: Presentation timestamp in 90kHz units
  // Returns true on success, false on failure (non-fatal errors may be logged and continue)
  virtual bool encodeFrame(const retrovue::buffer::Frame& frame, int64_t pts90k);

  // Close muxer and encoder, releasing all resources.
  // Safe to call multiple times.
  virtual void close();

  // Check if encoder is initialized and ready.
  virtual bool IsInitialized() const;

 private:
#ifdef RETROVUE_FFMPEG_AVAILABLE
  // FFmpeg encoder context
  AVCodecContext* codec_ctx_;
  
  // FFmpeg muxer context
  AVFormatContext* format_ctx_;
  
  // Video stream in muxer
  AVStream* video_stream_;
  
  // Encoder frame (reused for each frame)
  AVFrame* frame_;
  
  // Input frame buffer (for pixel format conversion)
  AVFrame* input_frame_;
  
  // Packet buffer (reused for each encoded packet)
  AVPacket* packet_;
  
  // Swscale context for format conversion
  SwsContext* sws_ctx_;
  
  // Frame dimensions
  int frame_width_;
  int frame_height_;
  
  // Input pixel format (defaults to YUV420P)
  AVPixelFormat input_pix_fmt_;
  
  // Flag to track if swscale context needs to be recreated
  bool sws_ctx_valid_;
  
  // Time base for video stream (1/90000 for MPEG-TS)
  AVRational time_base_;
  
  // Flag to track if header has been written
  bool header_written_;
  
  // Muxer options for PCR cadence configuration (FE-019)
  AVDictionary* muxer_opts_;
  
  // Custom AVIO write callback (for nonblocking mode)
  void* avio_opaque_;  // Opaque pointer passed to write callback
  int (*avio_write_callback_)(void* opaque, uint8_t* buf, int buf_size);  // C-style callback
  AVIOContext* custom_avio_ctx_;
  
  struct ContinuityState {
    uint8_t last_cc = 0;
    bool initialized = false;
  };
  struct ContinuityTestState {
    uint8_t last_cc = 0;
    bool initialized = false;
  };

  // TS packet tracking and validation
  // Continuity counter per PID (video PID typically 256)
  std::map<uint16_t, ContinuityState> continuity_counters_;
  uint64_t continuity_corrections_ = 0;
  std::map<uint16_t, ContinuityTestState> continuity_test_counters_;
  uint64_t continuity_test_packets_ = 0;
  uint64_t continuity_test_mismatches_ = 0;
  std::map<uint16_t, uint64_t> continuity_test_pid_packets_;
  std::map<uint16_t, uint64_t> continuity_test_pid_mismatches_;
  
  // Last PTS/DTS for monotonicity validation
  int64_t last_pts_90k_;
  int64_t last_dts_90k_;
  bool last_pts_valid_;
  bool last_dts_valid_;
  
  // PCR tracking (last PCR value and time)
  int64_t last_pcr_90k_;
  int64_t last_pcr_packet_time_us_;
  bool last_pcr_valid_;
  
  // Packet alignment buffer (for ensuring 188-byte TS packet boundaries)
  std::vector<uint8_t> packet_alignment_buffer_;
  
  // Stall detection
  int64_t last_write_time_us_;
  int64_t last_packet_time_us_;
  static constexpr int64_t kStallTimeoutUs = 1'000'000;  // 1 second
  
  // Helper methods for TS packet parsing and validation
  void ProcessTSPackets(uint8_t* data, size_t size);
  uint8_t ExtractContinuityCounter(const uint8_t* ts_packet);
  uint16_t ExtractPID(const uint8_t* ts_packet);
  bool ExtractPCR(const uint8_t* ts_packet, int64_t& pcr_90k);
  bool ValidatePacketAlignment(const uint8_t* data, size_t size);
  
  // Write callback wrapper that ensures packet alignment
  bool WriteWithAlignment(const uint8_t* data, size_t size);
  static int AVIOWriteThunk(void* opaque, uint8_t* buf, int buf_size);
  int HandleAVIOWrite(uint8_t* buf, int buf_size);
#endif

  const MpegTSPlayoutSinkConfig& config_;
  bool initialized_;
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_ENCODER_PIPELINE_HPP_

