// Repository: Retrovue-playout
// Component: FFmpeg Decoder
// Purpose: Real video decoding using libavformat/libavcodec.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_DECODE_FFMPEG_DECODER_H_
#define RETROVUE_DECODE_FFMPEG_DECODER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "retrovue/buffer/FrameRingBuffer.h"

// Forward declarations for FFmpeg types (avoids pulling in FFmpeg headers here)
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace retrovue::decode {

// DecoderConfig holds configuration for FFmpeg-based decoding.
struct DecoderConfig {
  std::string input_uri;        // File path or URI to decode
  int target_width;             // Target output width (for scaling)
  int target_height;            // Target output height (for scaling)
  bool hw_accel_enabled;        // Enable hardware acceleration if available
  int max_decode_threads;       // Maximum decoder threads (0 = auto)
  
  DecoderConfig()
      : target_width(1920),
        target_height(1080),
        hw_accel_enabled(false),
        max_decode_threads(0) {}
};

// DecoderStats tracks decoding performance and errors.
struct DecoderStats {
  uint64_t frames_decoded;
  uint64_t frames_dropped;
  uint64_t decode_errors;
  double average_decode_time_ms;
  double current_fps;
  
  DecoderStats()
      : frames_decoded(0),
        frames_dropped(0),
        decode_errors(0),
        average_decode_time_ms(0.0),
        current_fps(0.0) {}
};

// FFmpegDecoder decodes video files using libavformat and libavcodec.
//
// Features:
// - Supports H.264, HEVC, and other common codecs
// - Automatic format detection via libavformat
// - Optional scaling to target resolution
// - YUV420 output format
// - Frame timing from PTS
//
// Thread Safety:
// - Not thread-safe: Use from single decode thread
// - Outputs to thread-safe FrameRingBuffer
//
// Lifecycle:
// 1. Construct with config
// 2. Call Open() to initialize decoder
// 3. Call DecodeNextFrame() repeatedly
// 4. Call Close() or rely on destructor
//
// Error Handling:
// - Returns false on errors with stats updated
// - Supports recovery from transient decode errors
// - Tracks error count for monitoring
class FFmpegDecoder {
 public:
  explicit FFmpegDecoder(const DecoderConfig& config);
  ~FFmpegDecoder();

  // Disable copy and move
  FFmpegDecoder(const FFmpegDecoder&) = delete;
  FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

  // Opens the input file and initializes decoder.
  // Returns true on success, false on error.
  bool Open();

  // Decodes the next frame and pushes it to the output buffer.
  // Returns true if frame decoded successfully, false on error or EOF.
  bool DecodeNextFrame(buffer::FrameRingBuffer& output_buffer);

  // Decodes the next audio frame and pushes it to the output buffer.
  // Returns true if audio frame decoded successfully, false on error or EOF.
  bool DecodeNextAudioFrame(buffer::FrameRingBuffer& output_buffer);

  // Closes the decoder and releases resources.
  void Close();

  // Returns true if decoder is open and ready.
  bool IsOpen() const { return format_ctx_ != nullptr; }

  // Returns true if end of file reached.
  bool IsEOF() const { return eof_reached_; }

  // Gets current decoder statistics.
  const DecoderStats& GetStats() const { return stats_; }

  // Gets video stream information.
  int GetVideoWidth() const;
  int GetVideoHeight() const;
  double GetVideoFPS() const;
  double GetVideoDuration() const;

 private:
  // Finds the best video stream in the input.
  bool FindVideoStream();

  // Finds the best audio stream in the input.
  bool FindAudioStream();

  // Initializes the codec and codec context.
  bool InitializeCodec();

  // Initializes the audio codec and codec context.
  bool InitializeAudioCodec();

  // Initializes the scaler for resolution conversion.
  bool InitializeScaler();

  // Initializes the resampler for audio format conversion.
  bool InitializeResampler();

  // Reads and decodes a single frame.
  bool ReadAndDecodeFrame(buffer::Frame& output_frame);

  // Reads and decodes a single audio frame.
  bool ReadAndDecodeAudioFrame(buffer::AudioFrame& output_frame);

  // Converts AVFrame to our Frame format.
  bool ConvertFrame(AVFrame* av_frame, buffer::Frame& output_frame);

  // Converts AVFrame to our AudioFrame format (PCM S16 interleaved).
  bool ConvertAudioFrame(AVFrame* av_frame, buffer::AudioFrame& output_frame);

  // Updates decoder statistics.
  void UpdateStats(double decode_time_ms);

  DecoderConfig config_;
  DecoderStats stats_;

  // FFmpeg contexts (opaque pointers)
  AVFormatContext* format_ctx_;
  AVCodecContext* codec_ctx_;
  AVCodecContext* audio_codec_ctx_;
  AVFrame* frame_;
  AVFrame* scaled_frame_;
  AVFrame* audio_frame_;
  AVPacket* packet_;
  SwsContext* sws_ctx_;
  SwrContext* swr_ctx_;  // Audio resampler

  int video_stream_index_;
  int audio_stream_index_;
  bool eof_reached_;
  bool audio_eof_reached_;
  
  // Timing
  int64_t start_time_;
  double time_base_;
  int64_t audio_start_time_;
  double audio_time_base_;
};

}  // namespace retrovue::decode

#endif  // RETROVUE_DECODE_FFMPEG_DECODER_H_

