// Repository: Retrovue-playout
// Component: Encoder Pipeline
// Purpose: Owns FFmpeg encoder/muxer handles and manages encoding lifecycle.
// Copyright (c) 2025 RetroVue

#include "retrovue/playout_sinks/mpegts/EncoderPipeline.hpp"
#include "retrovue/playout_sinks/mpegts/MpegTSPlayoutSinkConfig.hpp"
#include "retrovue/buffer/FrameRingBuffer.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <sstream>
#include <cstring>
#include <algorithm>

#ifdef RETROVUE_FFMPEG_AVAILABLE
#include <libavutil/dict.h>
#include <libavutil/time.h>
#include <libavutil/mathematics.h>  // For av_rescale_q
#include <libavutil/log.h>  // For av_log_set_level
#endif

namespace retrovue::playout_sinks::mpegts {

#ifdef RETROVUE_FFMPEG_AVAILABLE

EncoderPipeline::EncoderPipeline(const MpegTSPlayoutSinkConfig& config)
    : config_(config),
      initialized_(false),
      codec_ctx_(nullptr),
      format_ctx_(nullptr),
      video_stream_(nullptr),
      frame_(nullptr),
      input_frame_(nullptr),
      packet_(nullptr),
      sws_ctx_(nullptr),
      frame_width_(0),
      frame_height_(0),
      input_pix_fmt_(AV_PIX_FMT_YUV420P),
      sws_ctx_valid_(false),
      header_written_(false),
      muxer_opts_(nullptr),
      avio_opaque_(nullptr),
      avio_write_callback_(nullptr),
      custom_avio_ctx_(nullptr),
      last_pts_90k_(0),
      last_dts_90k_(0),
      last_pts_valid_(false),
      last_dts_valid_(false),
      last_pcr_90k_(0),
      last_pcr_packet_time_us_(0),
      last_pcr_valid_(false),
      last_write_time_us_(0),
      last_packet_time_us_(0),
      continuity_corrections_(0),
      continuity_test_packets_(0),
      continuity_test_mismatches_(0) {
  time_base_.num = 1;
  time_base_.den = 90000;  // MPEG-TS timebase is 90kHz
}

EncoderPipeline::~EncoderPipeline() {
  close();
}

bool EncoderPipeline::open(const MpegTSPlayoutSinkConfig& config) {
  return open(config, nullptr, nullptr);
}

bool EncoderPipeline::open(const MpegTSPlayoutSinkConfig& config, 
                            void* opaque,
                            int (*write_callback)(void* opaque, uint8_t* buf, int buf_size)) {
  if (initialized_) {
    return true;  // Already initialized
  }

  if (config.stub_mode) {
    std::cout << "[EncoderPipeline] Stub mode enabled - skipping real encoding" << std::endl;
    initialized_ = true;
    return true;
  }

  continuity_counters_.clear();
  continuity_corrections_ = 0;
  continuity_test_counters_.clear();
  continuity_test_packets_ = 0;
  continuity_test_mismatches_ = 0;
  continuity_test_pid_packets_.clear();
  continuity_test_pid_mismatches_.clear();
  continuity_test_pid_packets_.clear();
  continuity_test_pid_mismatches_.clear();

  // Suppress FFmpeg warnings (e.g., "dts < pcr, TS is invalid") but keep errors visible
  av_log_set_level(AV_LOG_ERROR);

  // Store write callback and opaque pointer if provided
  avio_opaque_ = opaque;
  avio_write_callback_ = write_callback;

  std::string url;
  if (avio_write_callback_) {
    // Custom AVIO mode - use callback instead of URL
    url = "dummy://";  // Dummy URL for format context
    std::cout << "[EncoderPipeline] Opening encoder pipeline with custom AVIO (nonblocking mode)" << std::endl;
  } else {
    // Traditional URL mode - use TCP socket directly
    std::ostringstream url_stream;
    url_stream << "tcp://" << config.bind_host << ":" << config.port << "?listen=1";
    url = url_stream.str();
    std::cout << "[EncoderPipeline] Opening encoder pipeline: " << url << std::endl;
  }

  // Allocate format context
  int ret = avformat_alloc_output_context2(&format_ctx_, nullptr, "mpegts", url.c_str());
  if (ret < 0 || !format_ctx_) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    std::cerr << "[EncoderPipeline] Failed to allocate output context: " << errbuf << std::endl;
    return false;
  }
  
  // Configure MPEG-TS muxer for proper PCR cadence (FE-019: >= 19.4ms intervals, ~1750 in 90kHz)
  // FFmpeg's mpegts muxer inserts PCR automatically based on muxrate and max_delay
  // max_delay: maximum delay in microseconds (AV_TIME_BASE units) between PCR insertions
  // Setting max_delay to 100ms (100000 microseconds) ensures PCR is inserted at least every 100ms
  // This helps meet the >= 19.4ms requirement (PCR won't be too frequent)
  // muxrate: bitrate in bits per second (0 = VBR mode, PCR follows timestamps instead of bit clock)
  // Use AVDictionary to set muxer options (more reliable than av_opt_set)
  AVDictionary* muxer_opts = nullptr;
  // max_delay: maximum delay in microseconds (AV_TIME_BASE = 1,000,000)
  // 100ms = 100000 microseconds - ensures PCR at least every 100ms
  av_dict_set(&muxer_opts, "max_delay", "100000", 0);  // 100ms max delay
  // muxrate: Set to 0 for VBR mode - PCR will follow timestamps (PTS/DTS) instead of fixed bitrate clock
  av_dict_set(&muxer_opts, "muxrate", "0", 0);  // VBR mode
  // Store options for use in avformat_write_header
  muxer_opts_ = muxer_opts;
  
  // FE-017: Set MPEG-TS flags to reset continuity counters on each new stream
  // This ensures continuity counters start at 0 for each client connection
  // These flags ensure PAT/PMT are sent at start and continuity counters reset
  av_dict_set(&muxer_opts_, "mpegts_flags", "resend_headers", 0);
  av_dict_set(&muxer_opts_, "mpegts_flags", "+pat_pmt_at_frames", 0);
  
  // If using custom AVIO, set up custom write callback
  if (avio_write_callback_) {
    // Allocate smaller buffer for AVIO (16KB buffer) to reduce blocking risk
    // Smaller buffer means less data can accumulate before backpressure is felt
    const size_t buffer_size = 16 * 1024;  // 16KB instead of 64KB
    uint8_t* buffer = (uint8_t*)av_malloc(buffer_size);
    if (!buffer) {
      std::cerr << "[EncoderPipeline] Failed to allocate AVIO buffer" << std::endl;
      close();
      return false;
    }
    
    // Create custom AVIO context with write callback
    // Use the provided callback directly (not our wrapper)
    custom_avio_ctx_ = avio_alloc_context(
        buffer, buffer_size, 1, this, nullptr, &EncoderPipeline::AVIOWriteThunk, nullptr);
    if (!custom_avio_ctx_) {
      std::cerr << "[EncoderPipeline] Failed to allocate AVIO context" << std::endl;
      av_free(buffer);
      close();
      return false;
    }
    
    // Explicitly mark as non-blocking (required on some FFmpeg builds)
    custom_avio_ctx_->seekable = 0;
    
    // Set AVIO context on format context
    format_ctx_->pb = custom_avio_ctx_;
    format_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;
  }

  // Find H.264 encoder
  const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec) {
    std::cerr << "[EncoderPipeline] H.264 encoder not found" << std::endl;
    close();
    return false;
  }

  // Create video stream
  video_stream_ = avformat_new_stream(format_ctx_, codec);
  if (!video_stream_) {
    std::cerr << "[EncoderPipeline] Failed to create video stream" << std::endl;
    close();
    return false;
  }

  video_stream_->id = format_ctx_->nb_streams - 1;

  // Allocate codec context
  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_) {
    std::cerr << "[EncoderPipeline] Failed to allocate codec context" << std::endl;
    close();
    return false;
  }

  // Set codec parameters
  // Note: Frame dimensions will be set from first frame
  codec_ctx_->codec_id = AV_CODEC_ID_H264;
  codec_ctx_->codec_type = AVMEDIA_TYPE_VIDEO;
  codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
  codec_ctx_->bit_rate = config.bitrate;
  codec_ctx_->gop_size = config.gop_size;
  codec_ctx_->max_b_frames = 0;  // No B-frames for low latency
  codec_ctx_->time_base.num = 1;
  codec_ctx_->time_base.den = static_cast<int>(config.target_fps);
  codec_ctx_->framerate.num = static_cast<int>(config.target_fps);
  codec_ctx_->framerate.den = 1;

  // Set stream time base to 90kHz (MPEG-TS standard)
  video_stream_->time_base.num = 1;
  video_stream_->time_base.den = 90000;

  // Copy codec parameters to stream
  ret = avcodec_parameters_from_context(video_stream_->codecpar, codec_ctx_);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    std::cerr << "[EncoderPipeline] Failed to copy codec parameters: " << errbuf << std::endl;
    close();
    return false;
  }

  // Open codec (dimensions will be set from first frame)
  // We'll open it after we get the first frame dimensions
  codec_ctx_->width = 0;
  codec_ctx_->height = 0;

  // Allocate frame, input frame, and packet
  frame_ = av_frame_alloc();
  input_frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (!frame_ || !input_frame_ || !packet_) {
    std::cerr << "[EncoderPipeline] Failed to allocate frame, input_frame, or packet" << std::endl;
    close();
    return false;
  }

  initialized_ = true;
  std::cout << "[EncoderPipeline] Encoder pipeline initialized (will set dimensions from first frame)" << std::endl;
  return true;
}

bool EncoderPipeline::encodeFrame(const retrovue::buffer::Frame& frame, int64_t pts90k) {
  if (!initialized_) {
    return false;
  }

  if (config_.stub_mode) {
    // Stub mode: just log
    std::cout << "[EncoderPipeline] encodeFrame() - stub mode | PTS_us=" << frame.metadata.pts
              << " | size=" << frame.width << "x" << frame.height << std::endl;
    return true;
  }

  // Stall detection: check if encoder/muxer has stalled
  auto now = std::chrono::steady_clock::now();
  int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
      now.time_since_epoch()).count();
  
  if (last_packet_time_us_ > 0 && 
      (now_us - last_packet_time_us_) > kStallTimeoutUs) {
    std::cerr << "[EncoderPipeline] WARNING: Encoder/muxer stall detected | "
              << "time_since_last_packet=" << ((now_us - last_packet_time_us_) / 1000) << "ms" << std::endl;
    // Don't return false - allow recovery attempt
  }
  
  // Also check write callback stall
  if (last_write_time_us_ > 0 && 
      (now_us - last_write_time_us_) > kStallTimeoutUs) {
    std::cerr << "[EncoderPipeline] WARNING: Write callback stall detected | "
              << "time_since_last_write=" << ((now_us - last_write_time_us_) / 1000) << "ms" << std::endl;
  }

  // Check if codec needs to be opened (first frame or dimensions changed)
  if (!codec_ctx_->width || !codec_ctx_->height || 
      codec_ctx_->width != frame.width || codec_ctx_->height != frame.height) {
    
    // Close existing codec if already open
    if (codec_ctx_->width > 0 && codec_ctx_->height > 0) {
      avcodec_close(codec_ctx_);
      if (frame_->data[0]) {
        av_freep(&frame_->data[0]);
      }
      // Invalidate swscale context (will be recreated with new dimensions)
      if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
      }
      sws_ctx_valid_ = false;
    }
    
    // Free input frame buffer if it exists
    if (input_frame_->data[0]) {
      av_freep(&input_frame_->data[0]);
    }

    // Set dimensions
    codec_ctx_->width = frame.width;
    codec_ctx_->height = frame.height;
    frame_width_ = frame.width;
    frame_height_ = frame.height;

    // Update stream codec parameters
    video_stream_->codecpar->width = frame.width;
    video_stream_->codecpar->height = frame.height;

    // Re-copy codec parameters to stream (with new dimensions)
    int ret = avcodec_parameters_from_context(video_stream_->codecpar, codec_ctx_);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      std::cerr << "[EncoderPipeline] Failed to copy codec parameters: " << errbuf << std::endl;
      return false;
    }

    // Find encoder
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
      std::cerr << "[EncoderPipeline] H.264 encoder not found" << std::endl;
      return false;
    }

    // Set encoder options for low latency using AVDictionary
    // This is more reliable than av_opt_set and works with all FFmpeg versions
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "ultrafast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);

    // Open codec with options
    ret = avcodec_open2(codec_ctx_, codec, &opts);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      std::cerr << "[EncoderPipeline] Failed to open codec: " << errbuf << std::endl;
      av_dict_free(&opts);
      return false;
    }
    
    // Free options dictionary (options are now applied to codec context)
    // Note: avcodec_open2 consumes the options, so we free the dictionary
    av_dict_free(&opts);

    // Allocate frame buffer
    ret = av_image_alloc(frame_->data, frame_->linesize,
                         frame.width, frame.height, AV_PIX_FMT_YUV420P, 32);
    if (ret < 0) {
      std::cerr << "[EncoderPipeline] Failed to allocate frame buffer" << std::endl;
      return false;
    }

    frame_->width = frame.width;
    frame_->height = frame.height;
    frame_->format = AV_PIX_FMT_YUV420P;

    // Invalidate swscale context (will be recreated after input_frame_ is allocated)
    if (sws_ctx_) {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }
    sws_ctx_valid_ = false;

    // Write header if not already written
    if (!header_written_) {
      // Only open AVIO if using URL mode (not custom AVIO)
      if (!avio_write_callback_ && !(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&format_ctx_->pb, format_ctx_->url, AVIO_FLAG_WRITE);
        if (ret < 0) {
          char errbuf[AV_ERROR_MAX_STRING_SIZE];
          av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
          std::cerr << "[EncoderPipeline] Failed to open output: " << errbuf << std::endl;
          return false;
        }
      }

      // Write stream header (only on first frame)
      // Note: This may call the write callback, which should be non-blocking
      // If write callback returns EAGAIN, avformat_write_header should handle it
      // Use muxer_opts_ to configure PCR cadence (FE-019)
      ret = avformat_write_header(format_ctx_, &muxer_opts_);
      if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        
        // If header write fails with EAGAIN, it means write callback couldn't write
        // This shouldn't block, but log it and return false to retry later
        if (ret == AVERROR(EAGAIN)) {
          std::cerr << "[EncoderPipeline] Header write blocked (EAGAIN) - will retry on next frame" << std::endl;
          return false;  // Retry on next frame
        }
        
        std::cerr << "[EncoderPipeline] Failed to write header: " << errbuf << std::endl;
        return false;
      }
      header_written_ = true;
      std::cout << "[EncoderPipeline] Header written successfully" << std::endl;
    }

    std::cout << "[EncoderPipeline] Codec opened: " << frame.width << "x" << frame.height << std::endl;
  }

  // Map frame data directly into AVFrame without copying (per instructions)
  // Frame.data is YUV420 planar (Y, U, V planes stored contiguously)
  // We need to copy planes into frame_->data[] with proper linesize
  // since AVFrame expects separate plane pointers
  
  // Verify frame data size
  size_t y_size = frame.width * frame.height;
  size_t uv_size = (frame.width / 2) * (frame.height / 2);
  size_t expected_size = y_size + 2 * uv_size;
  
  if (frame.data.size() < expected_size) {
    std::cerr << "[EncoderPipeline] Frame data too small: got " << frame.data.size()
              << " bytes, expected " << expected_size << " bytes" << std::endl;
    return false;
  }

  // Copy Y, U, V planes directly from frame.data into frame_->data[]
  // Y plane: width * height bytes
  const uint8_t* y_plane = frame.data.data();
  for (int y = 0; y < frame.height; y++) {
    memcpy(frame_->data[0] + y * frame_->linesize[0],
           y_plane + y * frame.width,
           frame.width);
  }

  // U plane: (width/2) * (height/2) bytes
  const uint8_t* u_plane = frame.data.data() + y_size;
  for (int y = 0; y < frame.height / 2; y++) {
    memcpy(frame_->data[1] + y * frame_->linesize[1],
           u_plane + y * (frame.width / 2),
           frame.width / 2);
  }

  // V plane: (width/2) * (height/2) bytes
  const uint8_t* v_plane = frame.data.data() + y_size + uv_size;
  for (int y = 0; y < frame.height / 2; y++) {
    memcpy(frame_->data[2] + y * frame_->linesize[2],
           v_plane + y * (frame.width / 2),
           frame.width / 2);
  }

  // Set frame format explicitly (already YUV420P)
  frame_->format = AV_PIX_FMT_YUV420P;

  // Set frame PTS from pts90k (already in 90kHz units)
  // pts90k is monotonic and aligned with the producer's timeline
  // Convert from 90kHz timebase to codec timebase
  AVRational tb90k = {1, 90000};  // 90kHz timebase
  frame_->pts = av_rescale_q(pts90k, tb90k, codec_ctx_->time_base);

  // Send frame to encoder
  int send_ret = avcodec_send_frame(codec_ctx_, frame_);
  if (send_ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(send_ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    
    // EAGAIN means encoder is busy - try to drain packets first
    if (send_ret == AVERROR(EAGAIN)) {
      // Encoder is full - drain packets to make room
      // This handles encoder backpressure
      // Limit drain attempts to prevent infinite loops
      constexpr int max_drain_attempts = 5;
      int drain_attempts = 0;
      
      while (drain_attempts < max_drain_attempts) {
        int drain_ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (drain_ret == AVERROR(EAGAIN) || drain_ret == AVERROR_EOF) {
          break;  // No more packets available
        }
        if (drain_ret < 0) {
          // Error draining - continue anyway
          break;
        }
        
        drain_attempts++;
        
        // Write drained packet
        packet_->stream_index = video_stream_->index;
        av_packet_rescale_ts(packet_, codec_ctx_->time_base, video_stream_->time_base);
        
        // Debug: Log DTS/PTS for drained packets (throttled)
        static uint64_t drain_debug_count = 0;
        if (drain_debug_count++ % 100 == 0) {
          std::cerr << "[DEBUG] [drain] dts_90k=" << packet_->dts
                    << " pts_90k=" << packet_->pts << std::endl;
        }
        
        int write_ret = av_interleaved_write_frame(format_ctx_, packet_);
        if (write_ret < 0) {
          if (write_ret == AVERROR(EAGAIN)) {
            // Muxer backpressure - drop packet and break to prevent blocking
            std::cerr << "[EncoderPipeline] Muxer backpressure during drain - dropping packet" << std::endl;
            av_packet_unref(packet_);
            break;  // Stop draining to prevent blocking
          } else {
            // Log error but continue
            char write_errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(write_ret, write_errbuf, AV_ERROR_MAX_STRING_SIZE);
            std::cerr << "[EncoderPipeline] Error writing drained packet: " << write_errbuf << std::endl;
          }
        }
        
        av_packet_unref(packet_);
      }
      
      // Try sending frame again after draining
      send_ret = avcodec_send_frame(codec_ctx_, frame_);
      if (send_ret == AVERROR(EAGAIN)) {
        // Still full - frame will be processed on next iteration
        return true;  // Not an error, just backpressure
      }
    }
    
    if (send_ret < 0) {
      // Real error
      std::cerr << "[EncoderPipeline] Error sending frame: " << errbuf << std::endl;
      return false;
    }
  }

  // Receive encoded packets (may produce zero or more packets per input frame)
  // Handle encoder backpressure: EAGAIN means no packet available yet
  // Limit number of packets processed per frame to prevent infinite loops
  constexpr int max_packets_per_frame = 10;  // Safety limit
  int packets_processed = 0;
  
  while (packets_processed < max_packets_per_frame) {
    int recv_ret = avcodec_receive_packet(codec_ctx_, packet_);
    
    if (recv_ret == AVERROR(EAGAIN)) {
      // No packet available yet - this is normal (encoder needs more input)
      // This is backpressure, not an error
      break;
    }
    
    if (recv_ret == AVERROR_EOF) {
      // Encoder is flushed - no more packets
      break;
    }
    
    if (recv_ret < 0) {
      // Real error
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(recv_ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      std::cerr << "[EncoderPipeline] Error receiving packet: " << errbuf << std::endl;
      return false;
    }

    packets_processed++;

    // Packet received successfully
    // Set packet stream index and timestamp
    packet_->stream_index = video_stream_->index;
    
    // Convert PTS from codec timebase to stream timebase (90kHz)
    av_packet_rescale_ts(packet_, codec_ctx_->time_base, video_stream_->time_base);
    
    // Validate and enforce PTS/DTS strict monotonicity (FE-017)
    if (packet_->pts != AV_NOPTS_VALUE) {
      int64_t pts_90k = packet_->pts;
      
      if (last_pts_valid_) {
        if (pts_90k <= last_pts_90k_) {
          // Enforce strict monotonicity: increment PTS to be > last_pts
          pts_90k = last_pts_90k_ + 1;
          packet_->pts = pts_90k;
          static uint64_t pts_warning_count = 0;
          if (pts_warning_count++ % 100 == 0) {
            std::cerr << "[EncoderPipeline] PTS non-monotonic - adjusted to " << pts_90k << std::endl;
          }
        }
      }
      
      last_pts_90k_ = pts_90k;
      last_pts_valid_ = true;
    }
    
    if (packet_->dts != AV_NOPTS_VALUE) {
      int64_t dts_90k = packet_->dts;
      
      // Ensure DTS <= PTS (if both present)
      if (packet_->pts != AV_NOPTS_VALUE && dts_90k > packet_->pts) {
        // Adjust DTS to be <= PTS
        dts_90k = packet_->pts;
        packet_->dts = dts_90k;
        static uint64_t dts_warning_count = 0;
        if (dts_warning_count++ % 100 == 0) {
          std::cerr << "[EncoderPipeline] DTS > PTS - adjusted to " << dts_90k << std::endl;
        }
      }
      
      if (last_dts_valid_) {
        if (dts_90k <= last_dts_90k_) {
          // Enforce strict monotonicity: increment DTS to be > last_dts
          dts_90k = last_dts_90k_ + 1;
          packet_->dts = dts_90k;
          // Re-check DTS <= PTS after adjustment
          if (packet_->pts != AV_NOPTS_VALUE && dts_90k > packet_->pts) {
            dts_90k = packet_->pts;
            packet_->dts = dts_90k;
          }
          static uint64_t dts_warning_count = 0;
          if (dts_warning_count++ % 100 == 0) {
            std::cerr << "[EncoderPipeline] DTS non-monotonic - adjusted to " << dts_90k << std::endl;
          }
        }
      }
      
      last_dts_90k_ = dts_90k;
      last_dts_valid_ = true;
    }
    
    // Update packet time for stall detection
    auto now = std::chrono::steady_clock::now();
    last_packet_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();

    // Debug: Log DTS/PTS before writing (throttled to avoid spam)
    static uint64_t debug_log_count = 0;
    if (debug_log_count++ % 100 == 0) {
      std::cerr << "[DEBUG] dts_90k=" << packet_->dts
                << " pts_90k=" << packet_->pts << std::endl;
    }

    // Write packet to muxer
    // Note: Even though our callback always returns buf_size, av_interleaved_write_frame
    // might still block internally. However, with our callback implementation,
    // it should not block since we always pretend success.
    int write_ret = av_interleaved_write_frame(format_ctx_, packet_);
    
    if (write_ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(write_ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      
      // Handle muxer backpressure
      if (write_ret == AVERROR(EAGAIN)) {
        // Muxer would block - drop this packet and break to prevent infinite loop
        // The packet will be lost, but we can't block the timing loop
        std::cerr << "[EncoderPipeline] Muxer backpressure (EAGAIN) - dropping packet to prevent blocking" << std::endl;
        av_packet_unref(packet_);
        // Break out of loop to prevent infinite retry
        break;
      }
      
      // Other errors - log but continue (non-fatal)
      std::cerr << "[EncoderPipeline] Error writing packet: " << errbuf << std::endl;
      av_packet_unref(packet_);
      // Non-fatal: continue to next packet
      continue;
    }

    // Packet written successfully
    av_packet_unref(packet_);
  }
  
  if (packets_processed >= max_packets_per_frame) {
    std::cerr << "[EncoderPipeline] WARNING: Processed " << packets_processed 
              << " packets (limit reached) - may have more packets to process" << std::endl;
  }

  return true;
}

void EncoderPipeline::close() {
  if (!initialized_) {
    return;
  }

  if (config_.stub_mode) {
    std::cout << "[EncoderPipeline] close() - stub mode" << std::endl;
    initialized_ = false;
    return;
  }

#ifdef RETROVUE_FFMPEG_AVAILABLE
  // Free muxer options
  if (muxer_opts_) {
    av_dict_free(&muxer_opts_);
    muxer_opts_ = nullptr;
  }
  
  // Flush encoder
  if (codec_ctx_ && codec_ctx_->width > 0) {
    // Send NULL frame to flush
    avcodec_send_frame(codec_ctx_, nullptr);
    
    // Receive remaining packets
    while (true) {
      int ret = avcodec_receive_packet(codec_ctx_, packet_);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }
      if (ret >= 0) {
        packet_->stream_index = video_stream_->index;
        av_packet_rescale_ts(packet_, codec_ctx_->time_base, video_stream_->time_base);
        
        // Validate and enforce PTS/DTS monotonicity for flushed packets
        if (packet_->pts != AV_NOPTS_VALUE) {
          int64_t pts_90k = packet_->pts;
          if (last_pts_valid_ && pts_90k <= last_pts_90k_) {
            // Enforce strict monotonicity
            pts_90k = last_pts_90k_ + 1;
            packet_->pts = pts_90k;
          }
          last_pts_90k_ = pts_90k;
          last_pts_valid_ = true;
        }
        
        if (packet_->dts != AV_NOPTS_VALUE) {
          int64_t dts_90k = packet_->dts;
          // Ensure DTS <= PTS
          if (packet_->pts != AV_NOPTS_VALUE && dts_90k > packet_->pts) {
            dts_90k = packet_->pts;
            packet_->dts = dts_90k;
          }
          if (last_dts_valid_ && dts_90k <= last_dts_90k_) {
            dts_90k = last_dts_90k_ + 1;
            packet_->dts = dts_90k;
            // Re-check DTS <= PTS
            if (packet_->pts != AV_NOPTS_VALUE && dts_90k > packet_->pts) {
              dts_90k = packet_->pts;
              packet_->dts = dts_90k;
            }
          }
          last_dts_90k_ = dts_90k;
          last_dts_valid_ = true;
        }
        
        // Write flushed packet (check for errors)
        int write_ret = av_interleaved_write_frame(format_ctx_, packet_);
        if (write_ret < 0 && write_ret != AVERROR(EAGAIN)) {
          char errbuf[AV_ERROR_MAX_STRING_SIZE];
          av_strerror(write_ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
          std::cerr << "[EncoderPipeline] Error writing flushed packet: " << errbuf << std::endl;
        }
        
        av_packet_unref(packet_);
      }
    }
  }

  // Write trailer (finalizes TS stream, writes final PCR if needed)
  if (format_ctx_ && header_written_) {
    int ret = av_write_trailer(format_ctx_);
    if (ret < 0) {
      char errbuf[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
      std::cerr << "[EncoderPipeline] Error writing trailer: " << errbuf << std::endl;
    } else {
      std::cout << "[EncoderPipeline] Trailer written successfully" << std::endl;
    }
  }
  
  // Flush any remaining complete TS packets in alignment buffer (FE-020)
  // This ensures output ends on 188-byte packet boundary
  const size_t ts_packet_size = 188;
  if (!packet_alignment_buffer_.empty()) {
    size_t complete_packets = (packet_alignment_buffer_.size() / ts_packet_size) * ts_packet_size;
    
    if (complete_packets > 0) {
      // Fix continuity counters before writing
      ProcessTSPackets(packet_alignment_buffer_.data(), complete_packets);
      // Write complete packets
      if (avio_write_callback_ && avio_opaque_) {
        avio_write_callback_(avio_opaque_, packet_alignment_buffer_.data(), complete_packets);
      }
      packet_alignment_buffer_.erase(
          packet_alignment_buffer_.begin(),
          packet_alignment_buffer_.begin() + complete_packets);
    }
    
    // Pad remaining partial packet to 188-byte boundary (FE-020 requirement)
    if (!packet_alignment_buffer_.empty()) {
      size_t remaining = packet_alignment_buffer_.size();
      size_t padding_needed = ts_packet_size - remaining;
      
      // Pad with null bytes to complete the TS packet
      packet_alignment_buffer_.resize(ts_packet_size, 0);
      
      // Mark as null packet (PID 0x1FFF, sync byte 0x47)
      if (packet_alignment_buffer_.size() >= 4) {
        packet_alignment_buffer_[0] = 0x47;  // Sync byte
        packet_alignment_buffer_[1] = 0x1F;  // PID high byte (0x1FFF = null packet)
        packet_alignment_buffer_[2] = 0xFF;  // PID low byte
        packet_alignment_buffer_[3] = 0x10;  // Payload unit start indicator + adaptation field control
      }
      
      // Fix continuity counters before writing padded packet
      ProcessTSPackets(packet_alignment_buffer_.data(), ts_packet_size);
      // Write the padded complete packet
      if (avio_write_callback_ && avio_opaque_) {
        avio_write_callback_(avio_opaque_, packet_alignment_buffer_.data(), ts_packet_size);
      }
      
      packet_alignment_buffer_.clear();
    }
  }
  
  // FE-020: Ensure final output is aligned to 188-byte boundary
  // Since AVIO callback writes directly, we need to write a padding null packet
  // if the total output size is not a multiple of 188 bytes
  // Note: We can't easily track total bytes written through the callback,
  // so we write a null packet to ensure alignment (it will be ignored if already aligned)
  if (avio_write_callback_ && avio_opaque_) {
    // Write a null TS packet (188 bytes) to ensure alignment
    // Null packets (PID 0x1FFF) are valid and will be ignored by decoders
    uint8_t null_packet[188] = {0};
    null_packet[0] = 0x47;  // Sync byte
    null_packet[1] = 0x1F;  // PID high byte (0x1FFF = null packet)
    null_packet[2] = 0xFF;  // PID low byte
    null_packet[3] = 0x10;  // Payload unit start indicator + adaptation field control
    // Rest is zeros (null packet payload)
    
    avio_write_callback_(avio_opaque_, null_packet, 188);
  }

  // Close AVIO
  if (custom_avio_ctx_) {
    // Custom AVIO - free it
    if (custom_avio_ctx_->buffer) {
      av_freep(&custom_avio_ctx_->buffer);
    }
    avio_context_free(&custom_avio_ctx_);
    format_ctx_->pb = nullptr;
  } else if (format_ctx_ && format_ctx_->pb && !(format_ctx_->oformat->flags & AVFMT_NOFILE)) {
    // URL-based AVIO - close it
    avio_closep(&format_ctx_->pb);
  }
  
  avio_opaque_ = nullptr;
  avio_write_callback_ = nullptr;

  // Free resources
  if (frame_ && frame_->data[0]) {
    av_freep(&frame_->data[0]);
  }
  av_frame_free(&frame_);
  
  if (input_frame_ && input_frame_->data[0]) {
    av_freep(&input_frame_->data[0]);
  }
  av_frame_free(&input_frame_);
  
  av_packet_free(&packet_);
  avcodec_free_context(&codec_ctx_);
  avformat_free_context(format_ctx_);
  format_ctx_ = nullptr;

  if (continuity_corrections_ > 0) {
    std::cout << "[EncoderPipeline] Continuity corrections applied: "
              << continuity_corrections_ << std::endl;
  }
  if (continuity_test_packets_ > 0) {
    double rate = static_cast<double>(continuity_test_mismatches_) /
                  static_cast<double>(continuity_test_packets_);
    std::cout << "[EncoderPipeline] Continuity mismatches observed (raw): "
              << continuity_test_mismatches_ << "/" << continuity_test_packets_
              << " (" << rate * 100.0 << "%)" << std::endl;

    std::vector<std::pair<uint16_t, double>> pid_rates;
    pid_rates.reserve(continuity_test_pid_packets_.size());
    for (const auto& entry : continuity_test_pid_packets_) {
      uint16_t pid = entry.first;
      uint64_t total = entry.second;
      uint64_t mism = continuity_test_pid_mismatches_[pid];
      if (mism == 0 || total == 0) {
        continue;
      }
      double pid_rate = static_cast<double>(mism) / static_cast<double>(total);
      pid_rates.emplace_back(pid, pid_rate);
    }
    std::sort(pid_rates.begin(), pid_rates.end(),
              [](const auto& a, const auto& b) {
                return a.second > b.second;
              });
    if (!pid_rates.empty()) {
      std::cout << "[EncoderPipeline] Top continuity mismatch PIDs:" << std::endl;
      size_t limit = std::min<size_t>(pid_rates.size(), 5);
      for (size_t i = 0; i < limit; ++i) {
        uint16_t pid = pid_rates[i].first;
        uint64_t total = continuity_test_pid_packets_[pid];
        uint64_t mism = continuity_test_pid_mismatches_[pid];
        std::cout << "  PID " << pid << ": " << mism << "/" << total
                  << " (" << pid_rates[i].second * 100.0 << "%)" << std::endl;
      }
    }
  }

  continuity_counters_.clear();
  continuity_corrections_ = 0;
  continuity_test_counters_.clear();
  continuity_test_packets_ = 0;
  continuity_test_mismatches_ = 0;
  
  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }
  sws_ctx_valid_ = false;

  video_stream_ = nullptr;
  frame_width_ = 0;
  frame_height_ = 0;
  header_written_ = false;
#endif

  initialized_ = false;
  std::cout << "[EncoderPipeline] Encoder pipeline closed" << std::endl;
}

bool EncoderPipeline::IsInitialized() const {
  return initialized_;
}

#ifdef RETROVUE_FFMPEG_AVAILABLE
// Note: avioWriteCallback is no longer used - we use the callback directly

// Extract PID from TS packet (bytes 1-2)
uint16_t EncoderPipeline::ExtractPID(const uint8_t* ts_packet) {
  if (!ts_packet || ts_packet[0] != 0x47) {
    return 0xFFFF;  // Invalid
  }
  return ((ts_packet[1] & 0x1F) << 8) | ts_packet[2];
}

// Extract continuity counter from TS packet (byte 3, lower 4 bits)
uint8_t EncoderPipeline::ExtractContinuityCounter(const uint8_t* ts_packet) {
  if (!ts_packet || ts_packet[0] != 0x47) {
    return 0xFF;  // Invalid
  }
  return ts_packet[3] & 0x0F;
}

// Extract PCR from TS packet adaptation field (if present)
bool EncoderPipeline::ExtractPCR(const uint8_t* ts_packet, int64_t& pcr_90k) {
  if (!ts_packet || ts_packet[0] != 0x47) {
    return false;
  }
  
  // Check if adaptation field is present (bit 4 of byte 3)
  if (!(ts_packet[3] & 0x20)) {
    return false;  // No adaptation field
  }
  
  // Byte 4: adaptation_field_length
  uint8_t adaptation_field_length = ts_packet[4];
  if (adaptation_field_length == 0) {
    return false;  // No adaptation field data
  }
  
  // Byte 5: adaptation_field_control flags (bit 4 = PCR flag)
  if (!(ts_packet[5] & 0x10)) {
    return false;  // No PCR flag set
  }
  
  // PCR is 6 bytes starting at byte 6 (after adaptation_field_length and flags)
  // PCR = (33-bit base) * 300 + (9-bit extension)
  // PCR base ticks at 90kHz (each unit is 1/90000 second)
  // PCR extension refines that base to 27MHz resolution
  int64_t pcr_base = ((int64_t)ts_packet[6] << 25) |
                     ((int64_t)ts_packet[7] << 17) |
                     ((int64_t)ts_packet[8] << 9) |
                     ((int64_t)ts_packet[9] << 1) |
                     ((ts_packet[10] >> 7) & 0x01);
  int64_t pcr_ext = ((ts_packet[10] & 0x01) << 8) | ts_packet[11];
  
  // pcr_base is already in 90kHz units; keep integer precision in that domain.
  // (pcr_ext provides 27MHz sub-ticks; we ignore it for coarse 90kHz analysis.)
  (void)pcr_ext;
  pcr_90k = pcr_base;
  
  return true;
}

// Process TS packets and validate continuity counters
void EncoderPipeline::ProcessTSPackets(uint8_t* data, size_t size) {
  const size_t ts_packet_size = 188;
  size_t offset = 0;
  
  while (offset + ts_packet_size <= size) {
    uint8_t* ts_packet = data + offset;
    
    // Validate sync byte
    if (ts_packet[0] != 0x47) {
      std::cerr << "[EncoderPipeline] Invalid TS sync byte at offset " << offset << std::endl;
      offset += 1;  // Skip one byte and try to resync
      continue;
    }
    
    // Extract PID and continuity counter
    uint16_t pid = ExtractPID(ts_packet);
    uint8_t cc = ExtractContinuityCounter(ts_packet);
    uint8_t adaptation_field_control = (ts_packet[3] >> 4) & 0x03;
    bool has_adaptation = (adaptation_field_control & 0x02) != 0;
    bool has_payload = (adaptation_field_control & 0x01) != 0;
    
    // Null packets (PID 0x1FFF) do not participate in continuity tracking
    bool is_null_packet = (pid == 0x1FFF);
    
    // Check discontinuity indicator in adaptation field
    bool discontinuity_flag = false;
    if (has_adaptation && !is_null_packet) {
      size_t adaptation_offset = 4;
      if (adaptation_offset < ts_packet_size) {
        uint8_t adaptation_length = ts_packet[adaptation_offset];
        if (adaptation_length > 0 && adaptation_offset + 1 < ts_packet_size) {
          uint8_t adaptation_flags = ts_packet[adaptation_offset + 1];
          discontinuity_flag = (adaptation_flags & 0x80) != 0;
        }
      }
    }
    
    auto& test_state = continuity_test_counters_[pid];
    continuity_test_pid_packets_[pid]++;
    if (test_state.initialized) {
      uint8_t expected = (test_state.last_cc + 1) & 0x0F;
      if (cc != expected) {
        continuity_test_mismatches_++;
        continuity_test_pid_mismatches_[pid]++;
      }
      test_state.last_cc = cc;
    } else {
      test_state.last_cc = cc;
      test_state.initialized = true;
    }
    continuity_test_packets_++;

    if (!is_null_packet) {
      auto& state = continuity_counters_[pid];
      
      if (discontinuity_flag) {
        state.initialized = false;
      }
      
      if (!state.initialized) {
        state.last_cc = cc;
        state.initialized = true;
      } else if (has_payload) {
        uint8_t expected_cc = (state.last_cc + 1) & 0x0F;
        if (cc != expected_cc) {
          ts_packet[3] = (ts_packet[3] & 0xF0) | expected_cc;
          cc = expected_cc;
          continuity_corrections_++;
        }
        state.last_cc = cc;
      } else {
        // Adaptation-only packets must repeat the previous CC
        if (cc != state.last_cc) {
          ts_packet[3] = (ts_packet[3] & 0xF0) | state.last_cc;
          continuity_corrections_++;
        }
        // No change to last_cc when no payload
      }
    }
    
    // Check if payload unit start indicator is set (bit 6 of byte 1)
    bool payload_start = (ts_packet[1] & 0x40) != 0;
    
    // Track continuity counter
    if (!is_null_packet && payload_start) {
      // Logging for diagnostics when continuity had to be corrected repeatedly
      static uint64_t continuity_warning_count = 0;
      if (continuity_corrections_ > 0 && (continuity_warning_count++ % 500 == 0)) {
        std::cout << "[EncoderPipeline] Continuity correction applied | PID=" << pid
                  << " | CC=" << static_cast<int>(cc) << std::endl;
      }
    }
    
    // Extract and track PCR if present
    int64_t pcr_90k = 0;
    if (ExtractPCR(ts_packet, pcr_90k)) {
      // Debug: Log PCR values and differences
      static int64_t last_pcr_debug = -1;
      if (last_pcr_debug >= 0) {
        int64_t pcr_diff = pcr_90k - last_pcr_debug;
        static uint64_t pcr_debug_count = 0;
        if (pcr_debug_count++ % 100 == 0) {
          std::cerr << "[DEBUG] PCR_90k=" << pcr_90k
                    << " diff=" << pcr_diff
                    << " (~" << (pcr_diff / 90.0) << " ms)" << std::endl;
        }
      }
      last_pcr_debug = pcr_90k;
      
      auto now = std::chrono::steady_clock::now();
      int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          now.time_since_epoch()).count();
      
      if (last_pcr_valid_) {
        // Validate PCR cadence (should be ~40ms apart, allow 20-60ms range)
        int64_t pcr_diff = pcr_90k - last_pcr_90k_;
        int64_t time_diff_us = now_us - last_pcr_packet_time_us_;
        int64_t expected_pcr_diff = (time_diff_us * 90) / 1000;  // Convert us to 90kHz
        
        // Allow some tolerance (20-60ms)
        if (pcr_diff < (expected_pcr_diff * 20 / 40) || 
            pcr_diff > (expected_pcr_diff * 60 / 40)) {
          static uint64_t pcr_warning_count = 0;
          if (pcr_warning_count++ % 100 == 0) {
            std::cout << "[EncoderPipeline] PCR cadence warning | "
                      << "PCR_diff=" << pcr_diff << " | "
                      << "time_diff=" << (time_diff_us / 1000) << "ms" << std::endl;
          }
        }
      }
      
      last_pcr_90k_ = pcr_90k;
      last_pcr_packet_time_us_ = now_us;
      last_pcr_valid_ = true;
    }
    
    offset += ts_packet_size;
  }
}

// Validate that data is aligned on 188-byte TS packet boundaries
bool EncoderPipeline::ValidatePacketAlignment(const uint8_t* data, size_t size) {
  const size_t ts_packet_size = 188;
  
  // Check if size is multiple of 188
  if (size % ts_packet_size != 0) {
    return false;
  }
  
  // Check sync bytes at expected positions
  for (size_t i = 0; i < size; i += ts_packet_size) {
    if (data[i] != 0x47) {
      return false;
    }
  }
  
  return true;
}

// Write with packet alignment - ensures TS packets (188 bytes) are never split
bool EncoderPipeline::WriteWithAlignment(const uint8_t* data, size_t size) {
  const size_t ts_packet_size = 188;
  const size_t max_buffer_size = 1024 * 1024;  // 1MB max buffer size
  
  // Check if buffer would exceed max size
  if (packet_alignment_buffer_.size() + size > max_buffer_size) {
    // Buffer too large - drop oldest data to make room
    // This prevents unbounded growth when writes keep failing
    size_t drop_size = (packet_alignment_buffer_.size() + size) - max_buffer_size;
    drop_size = (drop_size / ts_packet_size + 1) * ts_packet_size;  // Round up to packet boundary
    if (drop_size < packet_alignment_buffer_.size()) {
      packet_alignment_buffer_.erase(
          packet_alignment_buffer_.begin(),
          packet_alignment_buffer_.begin() + drop_size);
      std::cerr << "[EncoderPipeline] Alignment buffer overflow - dropped " 
                << drop_size << " bytes" << std::endl;
    } else {
      packet_alignment_buffer_.clear();
    }
  }
  
  // Add to alignment buffer
  packet_alignment_buffer_.insert(packet_alignment_buffer_.end(), data, data + size);
  
  // Process complete TS packets
  size_t complete_packets = (packet_alignment_buffer_.size() / ts_packet_size) * ts_packet_size;
  
  if (complete_packets > 0) {
    // Fix continuity counters and validate before writing
    ProcessTSPackets(packet_alignment_buffer_.data(), complete_packets);
    
    // Write complete packets
    // Call the C-style callback directly (it always returns buf_size)
    if (avio_write_callback_ && avio_opaque_) {
      int result = avio_write_callback_(avio_opaque_, packet_alignment_buffer_.data(), complete_packets);
      (void)result;
      // Callback always returns buf_size (never blocks, never returns < buf_size)
      // So we can always proceed
    } else {
      // No callback - can't write
      return false;
    }
    
    // Remove written packets from buffer
    packet_alignment_buffer_.erase(
        packet_alignment_buffer_.begin(),
        packet_alignment_buffer_.begin() + complete_packets);
  }
  
  return true;
}

int EncoderPipeline::AVIOWriteThunk(void* opaque, uint8_t* buf, int buf_size) {
  if (opaque == nullptr) {
    return -1;
  }
  auto* pipeline = reinterpret_cast<EncoderPipeline*>(opaque);
  return pipeline->HandleAVIOWrite(buf, buf_size);
}

int EncoderPipeline::HandleAVIOWrite(uint8_t* buf, int buf_size) {
#ifdef RETROVUE_FFMPEG_AVAILABLE
  if (buf_size <= 0) {
    return 0;
  }

  if (!WriteWithAlignment(buf, static_cast<size_t>(buf_size))) {
    return -1;
  }

  last_write_time_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();

  return buf_size;
#else
  (void)buf;
  (void)buf_size;
  return 0;
#endif
}
#endif  // RETROVUE_FFMPEG_AVAILABLE

#else
// Stub implementations when FFmpeg is not available

EncoderPipeline::EncoderPipeline(const MpegTSPlayoutSinkConfig& config)
    : config_(config), initialized_(false) {
}

EncoderPipeline::~EncoderPipeline() {
  close();
}

bool EncoderPipeline::open(const MpegTSPlayoutSinkConfig& config) {
  if (initialized_) {
    return true;
  }

  std::cerr << "[EncoderPipeline] ERROR: FFmpeg not available. Rebuild with FFmpeg to enable real encoding." << std::endl;
  initialized_ = true;  // Allow stub mode to continue
  return true;
}

bool EncoderPipeline::encodeFrame(const retrovue::buffer::Frame& frame, int64_t pts90k) {
  if (!initialized_) {
    return false;
  }

  // Stub: just log
  std::cout << "[EncoderPipeline] encodeFrame() - FFmpeg not available | PTS=" << pts90k
            << " | size=" << frame.width << "x" << frame.height << std::endl;
  return true;
}

void EncoderPipeline::close() {
  if (!initialized_) {
    return;
  }

  std::cout << "[EncoderPipeline] close() - FFmpeg not available" << std::endl;
  initialized_ = false;
}

bool EncoderPipeline::IsInitialized() const {
  return initialized_;
}

#endif  // RETROVUE_FFMPEG_AVAILABLE

}  // namespace retrovue::playout_sinks::mpegts
