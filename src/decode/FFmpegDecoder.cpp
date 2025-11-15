// Repository: Retrovue-playout
// Component: FFmpeg Decoder
// Purpose: Real video decoding using libavformat/libavcodec.
// Copyright (c) 2025 RetroVue

#include "retrovue/decode/FFmpegDecoder.h"

#include <chrono>
#include <iostream>

#ifdef RETROVUE_FFMPEG_AVAILABLE
// FFmpeg C headers
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/log.h>  // For av_log_set_level
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#endif

namespace retrovue::decode {

#ifndef RETROVUE_FFMPEG_AVAILABLE
// Stub implementations when FFmpeg is not available

FFmpegDecoder::FFmpegDecoder(const DecoderConfig& config)
    : config_(config),
      format_ctx_(nullptr),
      codec_ctx_(nullptr),
      frame_(nullptr),
      scaled_frame_(nullptr),
      packet_(nullptr),
      sws_ctx_(nullptr),
      video_stream_index_(-1),
      eof_reached_(false),
      start_time_(0),
      time_base_(0.0) {
}

FFmpegDecoder::~FFmpegDecoder() {
}

bool FFmpegDecoder::Open() {
  std::cerr << "[FFmpegDecoder] ERROR: FFmpeg not available. Rebuild with FFmpeg to enable real decoding." << std::endl;
  return false;
}

bool FFmpegDecoder::DecodeNextFrame(buffer::FrameRingBuffer& output_buffer) {
  return false;
}

bool FFmpegDecoder::DecodeNextAudioFrame(buffer::FrameRingBuffer& output_buffer) {
  return false;
}

void FFmpegDecoder::Close() {
}

int FFmpegDecoder::GetVideoWidth() const { return 0; }
int FFmpegDecoder::GetVideoHeight() const { return 0; }
double FFmpegDecoder::GetVideoFPS() const { return 0.0; }
double FFmpegDecoder::GetVideoDuration() const { return 0.0; }

bool FFmpegDecoder::FindVideoStream() { return false; }
bool FFmpegDecoder::FindAudioStream() { return false; }
bool FFmpegDecoder::InitializeCodec() { return false; }
bool FFmpegDecoder::InitializeAudioCodec() { return false; }
bool FFmpegDecoder::InitializeScaler() { return false; }
bool FFmpegDecoder::InitializeResampler() { return false; }
bool FFmpegDecoder::ReadAndDecodeFrame(buffer::Frame& output_frame) { return false; }
bool FFmpegDecoder::ReadAndDecodeAudioFrame(buffer::AudioFrame& output_frame) { return false; }
bool FFmpegDecoder::ConvertFrame(AVFrame* av_frame, buffer::Frame& output_frame) { return false; }
bool FFmpegDecoder::ConvertAudioFrame(AVFrame* av_frame, buffer::AudioFrame& output_frame) { return false; }
void FFmpegDecoder::UpdateStats(double decode_time_ms) {}

#else
// Real implementations when FFmpeg is available

FFmpegDecoder::FFmpegDecoder(const DecoderConfig& config)
    : config_(config),
      format_ctx_(nullptr),
      codec_ctx_(nullptr),
      frame_(nullptr),
      scaled_frame_(nullptr),
      packet_(nullptr),
      sws_ctx_(nullptr),
      video_stream_index_(-1),
      eof_reached_(false),
      start_time_(0),
      time_base_(0.0) {
}

FFmpegDecoder::~FFmpegDecoder() {
  Close();
}

bool FFmpegDecoder::Open() {
  std::cout << "[FFmpegDecoder] Opening: " << config_.input_uri << std::endl;

  // Suppress FFmpeg warnings but keep errors visible
  av_log_set_level(AV_LOG_ERROR);

  // Allocate format context
  format_ctx_ = avformat_alloc_context();
  if (!format_ctx_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate format context" << std::endl;
    return false;
  }

  // Open input file
  if (avformat_open_input(&format_ctx_, config_.input_uri.c_str(), nullptr, nullptr) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to open input: " << config_.input_uri << std::endl;
    avformat_free_context(format_ctx_);
    format_ctx_ = nullptr;
    return false;
  }

  // Retrieve stream information
  if (avformat_find_stream_info(format_ctx_, nullptr) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to find stream info" << std::endl;
    Close();
    return false;
  }

  // Find video stream
  if (!FindVideoStream()) {
    std::cerr << "[FFmpegDecoder] No video stream found" << std::endl;
    Close();
    return false;
  }

  // Find audio stream (optional)
  FindAudioStream();

  // Initialize codec
  if (!InitializeCodec()) {
    std::cerr << "[FFmpegDecoder] Failed to initialize codec" << std::endl;
    Close();
    return false;
  }

  // Initialize audio codec (if audio stream found)
  if (audio_stream_index_ >= 0) {
    if (!InitializeAudioCodec()) {
      std::cerr << "[FFmpegDecoder] Failed to initialize audio codec" << std::endl;
      // Continue without audio
      audio_stream_index_ = -1;
    } else {
      if (!InitializeResampler()) {
        std::cerr << "[FFmpegDecoder] Failed to initialize audio resampler" << std::endl;
        // Continue without audio
        audio_stream_index_ = -1;
      }
    }
  }

  // Initialize scaler
  if (!InitializeScaler()) {
    std::cerr << "[FFmpegDecoder] Failed to initialize scaler" << std::endl;
    Close();
    return false;
  }

  // Allocate packet
  packet_ = av_packet_alloc();
  if (!packet_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate packet" << std::endl;
    Close();
    return false;
  }

  std::cout << "[FFmpegDecoder] Opened successfully: " << GetVideoWidth() << "x" 
            << GetVideoHeight() << " @ " << GetVideoFPS() << " fps" << std::endl;

  return true;
}

bool FFmpegDecoder::DecodeNextFrame(buffer::FrameRingBuffer& output_buffer) {
  if (!IsOpen()) {
    return false;
  }

  if (eof_reached_) {
    return false;
  }

  auto start_time = std::chrono::steady_clock::now();

  buffer::Frame output_frame;
  if (!ReadAndDecodeFrame(output_frame)) {
    return false;
  }

  // Try to push to buffer
  if (!output_buffer.Push(output_frame)) {
    stats_.frames_dropped++;
    return false;  // Buffer full
  }

  auto end_time = std::chrono::steady_clock::now();
  double decode_time_ms = std::chrono::duration<double, std::milli>(
      end_time - start_time).count();
  
  UpdateStats(decode_time_ms);

  return true;
}

void FFmpegDecoder::Close() {
  std::cout << "[FFmpegDecoder] Closing decoder" << std::endl;

  if (sws_ctx_) {
    sws_freeContext(sws_ctx_);
    sws_ctx_ = nullptr;
  }

  if (scaled_frame_) {
    av_frame_free(&scaled_frame_);
  }

  if (frame_) {
    av_frame_free(&frame_);
  }

  if (packet_) {
    av_packet_free(&packet_);
  }

  if (swr_ctx_) {
    swr_free(&swr_ctx_);
  }

  if (audio_frame_) {
    av_frame_free(&audio_frame_);
  }

  if (audio_codec_ctx_) {
    avcodec_free_context(&audio_codec_ctx_);
  }

  if (codec_ctx_) {
    avcodec_free_context(&codec_ctx_);
  }

  if (format_ctx_) {
    avformat_close_input(&format_ctx_);
  }

  video_stream_index_ = -1;
  audio_stream_index_ = -1;
  eof_reached_ = false;
  audio_eof_reached_ = false;
}

int FFmpegDecoder::GetVideoWidth() const {
  if (!codec_ctx_) return 0;
  return codec_ctx_->width;
}

int FFmpegDecoder::GetVideoHeight() const {
  if (!codec_ctx_) return 0;
  return codec_ctx_->height;
}

double FFmpegDecoder::GetVideoFPS() const {
  if (!format_ctx_ || video_stream_index_ < 0) return 0.0;
  
  AVStream* stream = format_ctx_->streams[video_stream_index_];
  AVRational fps = stream->avg_frame_rate;
  
  if (fps.den == 0) return 0.0;
  return static_cast<double>(fps.num) / static_cast<double>(fps.den);
}

double FFmpegDecoder::GetVideoDuration() const {
  if (!format_ctx_) return 0.0;
  
  if (format_ctx_->duration != AV_NOPTS_VALUE) {
    return static_cast<double>(format_ctx_->duration) / AV_TIME_BASE;
  }
  
  return 0.0;
}

bool FFmpegDecoder::FindVideoStream() {
  for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
    if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_index_ = i;
      
      AVStream* stream = format_ctx_->streams[i];
      time_base_ = av_q2d(stream->time_base);
      start_time_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
      
      return true;
    }
  }
  
  return false;
}

bool FFmpegDecoder::FindAudioStream() {
  for (unsigned int i = 0; i < format_ctx_->nb_streams; i++) {
    if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_stream_index_ = i;
      
      AVStream* stream = format_ctx_->streams[i];
      audio_time_base_ = av_q2d(stream->time_base);
      audio_start_time_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
      
      return true;
    }
  }
  
  return false;
}

bool FFmpegDecoder::InitializeCodec() {
  AVStream* stream = format_ctx_->streams[video_stream_index_];
  AVCodecParameters* codecpar = stream->codecpar;

  // Find decoder
  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    std::cerr << "[FFmpegDecoder] Codec not found: " << codecpar->codec_id << std::endl;
    return false;
  }

  // Allocate codec context
  codec_ctx_ = avcodec_alloc_context3(codec);
  if (!codec_ctx_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate codec context" << std::endl;
    return false;
  }

  // Copy codec parameters
  if (avcodec_parameters_to_context(codec_ctx_, codecpar) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to copy codec parameters" << std::endl;
    return false;
  }

  // Set threading
  if (config_.max_decode_threads > 0) {
    codec_ctx_->thread_count = config_.max_decode_threads;
  }
  codec_ctx_->thread_type = FF_THREAD_FRAME;

  // Open codec
  if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to open codec" << std::endl;
    return false;
  }

  // Allocate frames
  frame_ = av_frame_alloc();
  scaled_frame_ = av_frame_alloc();
  
  if (!frame_ || !scaled_frame_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate frames" << std::endl;
    return false;
  }

  return true;
}

bool FFmpegDecoder::InitializeScaler() {
  // Get source format
  int src_width = codec_ctx_->width;
  int src_height = codec_ctx_->height;
  AVPixelFormat src_format = codec_ctx_->pix_fmt;

  // Target format: YUV420P
  int dst_width = config_.target_width;
  int dst_height = config_.target_height;
  AVPixelFormat dst_format = AV_PIX_FMT_YUV420P;

  // Create scaler context
  sws_ctx_ = sws_getContext(
      src_width, src_height, src_format,
      dst_width, dst_height, dst_format,
      SWS_BILINEAR, nullptr, nullptr, nullptr);

  if (!sws_ctx_) {
    std::cerr << "[FFmpegDecoder] Failed to create scaler context" << std::endl;
    return false;
  }

  // Allocate buffer for scaled frame
  if (av_image_alloc(scaled_frame_->data, scaled_frame_->linesize,
                     dst_width, dst_height, dst_format, 32) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to allocate scaled frame buffer" << std::endl;
    return false;
  }

  scaled_frame_->width = dst_width;
  scaled_frame_->height = dst_height;
  scaled_frame_->format = dst_format;

  return true;
}

bool FFmpegDecoder::InitializeAudioCodec() {
  if (audio_stream_index_ < 0) {
    return false;
  }

  AVStream* stream = format_ctx_->streams[audio_stream_index_];
  AVCodecParameters* codecpar = stream->codecpar;

  // Find decoder
  const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
  if (!codec) {
    std::cerr << "[FFmpegDecoder] Audio codec not found: " << codecpar->codec_id << std::endl;
    return false;
  }

  // Allocate codec context
  audio_codec_ctx_ = avcodec_alloc_context3(codec);
  if (!audio_codec_ctx_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate audio codec context" << std::endl;
    return false;
  }

  // Copy codec parameters
  if (avcodec_parameters_to_context(audio_codec_ctx_, codecpar) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to copy audio codec parameters" << std::endl;
    return false;
  }

  // Open codec
  if (avcodec_open2(audio_codec_ctx_, codec, nullptr) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to open audio codec" << std::endl;
    return false;
  }

  // Allocate audio frame
  audio_frame_ = av_frame_alloc();
  if (!audio_frame_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate audio frame" << std::endl;
    return false;
  }

  return true;
}

bool FFmpegDecoder::InitializeResampler() {
  if (!audio_codec_ctx_ || audio_stream_index_ < 0) {
    return false;
  }

  // Source format (from decoder) - use modern AVChannelLayout API
  AVChannelLayout src_ch_layout;
  av_channel_layout_uninit(&src_ch_layout);  // Initialize to empty state
  int src_nb_channels = 0;
  
  // Try to use the new ch_layout field first
  if (audio_codec_ctx_->ch_layout.nb_channels > 0) {
    // Use the channel layout from codec context if available
    if (av_channel_layout_copy(&src_ch_layout, &audio_codec_ctx_->ch_layout) < 0) {
      std::cerr << "[FFmpegDecoder] Failed to copy source channel layout" << std::endl;
      return false;
    }
    src_nb_channels = src_ch_layout.nb_channels;
  } else {
    // Fallback: use deprecated channels field and create default layout
    src_nb_channels = audio_codec_ctx_->channels;
    if (src_nb_channels <= 0) {
      std::cerr << "[FFmpegDecoder] Invalid channel count" << std::endl;
      return false;
    }
    // Create default channel layout based on channel count
    av_channel_layout_default(&src_ch_layout, src_nb_channels);
    if (src_ch_layout.nb_channels == 0) {
      std::cerr << "[FFmpegDecoder] Failed to create default channel layout" << std::endl;
      return false;
    }
  }
  AVSampleFormat src_sample_fmt = audio_codec_ctx_->sample_fmt;
  int src_sample_rate = audio_codec_ctx_->sample_rate;

  // Target format: S16 interleaved, stereo, 48kHz
  AVChannelLayout dst_ch_layout;
  av_channel_layout_uninit(&dst_ch_layout);  // Initialize to empty state
  uint64_t dst_ch_mask = AV_CH_LAYOUT_STEREO;
  if (av_channel_layout_from_mask(&dst_ch_layout, dst_ch_mask) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to create destination channel layout" << std::endl;
    av_channel_layout_uninit(&src_ch_layout);
    return false;
  }
  AVSampleFormat dst_sample_fmt = AV_SAMPLE_FMT_S16;
  int dst_sample_rate = 48000;

  // Create resampler context using modern API
  swr_ctx_ = swr_alloc();
  if (!swr_ctx_) {
    std::cerr << "[FFmpegDecoder] Failed to allocate resampler context" << std::endl;
    av_channel_layout_uninit(&src_ch_layout);
    av_channel_layout_uninit(&dst_ch_layout);
    return false;
  }

  // Set options using modern API
  if (swr_alloc_set_opts2(&swr_ctx_,
                          &dst_ch_layout, dst_sample_fmt, dst_sample_rate,
                          &src_ch_layout, src_sample_fmt, src_sample_rate,
                          0, nullptr) != 0) {
    std::cerr << "[FFmpegDecoder] Failed to set resampler options" << std::endl;
    swr_free(&swr_ctx_);
    av_channel_layout_uninit(&src_ch_layout);
    av_channel_layout_uninit(&dst_ch_layout);
    return false;
  }

  // Clean up channel layouts (swr_alloc_set_opts2 copies them)
  av_channel_layout_uninit(&src_ch_layout);
  av_channel_layout_uninit(&dst_ch_layout);

  // Initialize resampler
  if (swr_init(swr_ctx_) < 0) {
    std::cerr << "[FFmpegDecoder] Failed to initialize resampler" << std::endl;
    swr_free(&swr_ctx_);
    return false;
  }

  return true;
}

bool FFmpegDecoder::ReadAndDecodeFrame(buffer::Frame& output_frame) {
  while (true) {
    // Read packet
    int ret = av_read_frame(format_ctx_, packet_);
    
    if (ret == AVERROR_EOF) {
      eof_reached_ = true;
      return false;
    }
    
    if (ret < 0) {
      stats_.decode_errors++;
      av_packet_unref(packet_);
      return false;
    }

    // Check if packet is from video stream
    if (packet_->stream_index != video_stream_index_) {
      av_packet_unref(packet_);
      continue;
    }

    // Send packet to decoder
    ret = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    
    if (ret < 0) {
      stats_.decode_errors++;
      return false;
    }

    // Receive decoded frame
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    
    if (ret == AVERROR(EAGAIN)) {
      continue;  // Need more packets
    }
    
    if (ret < 0) {
      stats_.decode_errors++;
      return false;
    }

    // Successfully decoded a frame
    return ConvertFrame(frame_, output_frame);
  }
}

bool FFmpegDecoder::ConvertFrame(AVFrame* av_frame, buffer::Frame& output_frame) {
  // Scale frame
  sws_scale(sws_ctx_, 
            av_frame->data, av_frame->linesize, 0, codec_ctx_->height,
            scaled_frame_->data, scaled_frame_->linesize);

  // Set frame metadata
  output_frame.width = config_.target_width;
  output_frame.height = config_.target_height;

  // Calculate PTS in seconds
  int64_t pts = av_frame->pts != AV_NOPTS_VALUE ? av_frame->pts : av_frame->best_effort_timestamp;
  output_frame.metadata.pts = pts;
  output_frame.metadata.dts = av_frame->pkt_dts;
  // Use duration field (preferred) or fallback to pkt_duration for older FFmpeg versions
  int64_t frame_duration = av_frame->duration != AV_NOPTS_VALUE ? av_frame->duration : av_frame->pkt_duration;
  output_frame.metadata.duration = static_cast<double>(frame_duration) * time_base_;
  output_frame.metadata.asset_uri = config_.input_uri;

  // Copy YUV420 data
  int y_size = config_.target_width * config_.target_height;
  int uv_size = (config_.target_width / 2) * (config_.target_height / 2);
  int total_size = y_size + 2 * uv_size;

  output_frame.data.resize(total_size);

  // Copy Y plane
  uint8_t* dst = output_frame.data.data();
  for (int y = 0; y < config_.target_height; y++) {
    memcpy(dst + y * config_.target_width,
           scaled_frame_->data[0] + y * scaled_frame_->linesize[0],
           config_.target_width);
  }

  // Copy U plane
  dst += y_size;
  for (int y = 0; y < config_.target_height / 2; y++) {
    memcpy(dst + y * (config_.target_width / 2),
           scaled_frame_->data[1] + y * scaled_frame_->linesize[1],
           config_.target_width / 2);
  }

  // Copy V plane
  dst += uv_size;
  for (int y = 0; y < config_.target_height / 2; y++) {
    memcpy(dst + y * (config_.target_width / 2),
           scaled_frame_->data[2] + y * scaled_frame_->linesize[2],
           config_.target_width / 2);
  }

  return true;
}

bool FFmpegDecoder::DecodeNextAudioFrame(buffer::FrameRingBuffer& output_buffer) {
  if (!IsOpen() || audio_stream_index_ < 0) {
    return false;
  }

  if (audio_eof_reached_) {
    return false;
  }

  buffer::AudioFrame output_audio_frame;
  if (!ReadAndDecodeAudioFrame(output_audio_frame)) {
    return false;
  }

  // Try to push to buffer
  if (!output_buffer.PushAudioFrame(output_audio_frame)) {
    stats_.frames_dropped++;
    return false;  // Buffer full
  }

  return true;
}

bool FFmpegDecoder::ReadAndDecodeAudioFrame(buffer::AudioFrame& output_frame) {
  if (audio_stream_index_ < 0) {
    return false;
  }

  while (true) {
    // Read packet
    int ret = av_read_frame(format_ctx_, packet_);
    
    if (ret == AVERROR_EOF) {
      audio_eof_reached_ = true;
      return false;
    }
    
    if (ret < 0) {
      stats_.decode_errors++;
      av_packet_unref(packet_);
      return false;
    }

    // Check if packet is from audio stream
    if (packet_->stream_index != audio_stream_index_) {
      av_packet_unref(packet_);
      continue;
    }

    // Send packet to decoder
    ret = avcodec_send_packet(audio_codec_ctx_, packet_);
    av_packet_unref(packet_);
    
    if (ret < 0) {
      stats_.decode_errors++;
      return false;
    }

    // Receive decoded frame
    ret = avcodec_receive_frame(audio_codec_ctx_, audio_frame_);
    
    if (ret == AVERROR(EAGAIN)) {
      continue;  // Need more packets
    }
    
    if (ret < 0) {
      stats_.decode_errors++;
      return false;
    }

    // Successfully decoded an audio frame
    return ConvertAudioFrame(audio_frame_, output_frame);
  }
}

bool FFmpegDecoder::ConvertAudioFrame(AVFrame* av_frame, buffer::AudioFrame& output_frame) {
  if (!swr_ctx_) {
    return false;
  }

  // Calculate number of output samples
  int64_t delay = swr_get_delay(swr_ctx_, av_frame->sample_rate);
  int64_t out_samples = av_rescale_rnd(delay + av_frame->nb_samples,
                                        48000, av_frame->sample_rate,
                                        AV_ROUND_UP);

  // Allocate output buffer (S16 interleaved, stereo)
  int out_channels = 2;
  int out_sample_size = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
  int out_buffer_size = out_samples * out_channels * out_sample_size;
  
  output_frame.data.resize(out_buffer_size);
  uint8_t* out_data[1] = { output_frame.data.data() };

  // Resample
  int samples_converted = swr_convert(swr_ctx_,
                                       out_data, out_samples,
                                       const_cast<const uint8_t**>(av_frame->data), av_frame->nb_samples);

  if (samples_converted < 0) {
    std::cerr << "[FFmpegDecoder] Audio resampling failed" << std::endl;
    return false;
  }

  // Update output frame metadata
  output_frame.sample_rate = 48000;
  output_frame.channels = 2;
  output_frame.nb_samples = samples_converted;

  // Calculate PTS in microseconds
  int64_t pts = av_frame->pts != AV_NOPTS_VALUE ? av_frame->pts : av_frame->best_effort_timestamp;
  if (pts != AV_NOPTS_VALUE) {
    // Convert from stream timebase to microseconds
    output_frame.pts_us = static_cast<int64_t>((pts - audio_start_time_) * audio_time_base_ * 1'000'000.0);
  } else {
    output_frame.pts_us = 0;
  }

  // Resize data to actual converted size
  output_frame.data.resize(samples_converted * out_channels * out_sample_size);

  return true;
}

void FFmpegDecoder::UpdateStats(double decode_time_ms) {
  stats_.frames_decoded++;

  // Update average decode time (exponential moving average)
  const double alpha = 0.1;
  stats_.average_decode_time_ms = 
      alpha * decode_time_ms + (1.0 - alpha) * stats_.average_decode_time_ms;

  // Calculate current FPS
  if (stats_.average_decode_time_ms > 0.0) {
    stats_.current_fps = 1000.0 / stats_.average_decode_time_ms;
  }
}

#endif  // RETROVUE_FFMPEG_AVAILABLE

}  // namespace retrovue::decode

