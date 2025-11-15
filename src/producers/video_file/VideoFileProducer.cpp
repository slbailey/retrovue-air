// Repository: Retrovue-playout
// Component: Video File Producer
// Purpose: Self-contained decoder that reads and decodes video files, producing decoded YUV420 frames.
// Copyright (c) 2025 RetroVue

#include "retrovue/producers/video_file/VideoFileProducer.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#ifdef RETROVUE_FFMPEG_AVAILABLE
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#endif

#include "retrovue/timing/MasterClock.h"

namespace retrovue::producers::video_file
{

  namespace
  {
    constexpr int64_t kProducerBackoffUs = 10'000; // 10ms backoff when buffer is full
    constexpr int64_t kMicrosecondsPerSecond = 1'000'000;
  }

  VideoFileProducer::VideoFileProducer(
      const ProducerConfig &config,
      buffer::FrameRingBuffer &output_buffer,
      std::shared_ptr<timing::MasterClock> clock,
      ProducerEventCallback event_callback)
      : config_(config),
        output_buffer_(output_buffer),
        master_clock_(clock),
        event_callback_(event_callback),
        state_(ProducerState::STOPPED),
        stop_requested_(false),
        teardown_requested_(false),
        frames_produced_(0),
        buffer_full_count_(0),
        decode_errors_(0),
        drain_timeout_(std::chrono::milliseconds(0)),
        format_ctx_(nullptr),
        codec_ctx_(nullptr),
        frame_(nullptr),
        scaled_frame_(nullptr),
        packet_(nullptr),
        sws_ctx_(nullptr),
        video_stream_index_(-1),
        decoder_initialized_(false),
        eof_reached_(false),
        time_base_(0.0),
        last_pts_us_(0),
        last_decoded_frame_pts_us_(0),
        first_frame_pts_us_(0),
        playback_start_utc_us_(0),
        stub_pts_counter_(0),
        frame_interval_us_(static_cast<int64_t>(std::round(kMicrosecondsPerSecond / config.target_fps))),
        next_stub_deadline_utc_(0)
  {
  }

  VideoFileProducer::~VideoFileProducer()
  {
    stop();
    CloseDecoder();
  }

  void VideoFileProducer::SetState(ProducerState new_state)
  {
    ProducerState old_state = state_.exchange(new_state, std::memory_order_acq_rel);
    if (old_state != new_state)
    {
      std::ostringstream msg;
      msg << "state=" << static_cast<int>(new_state);
      EmitEvent("state_change", msg.str());
    }
  }

  void VideoFileProducer::EmitEvent(const std::string &event_type, const std::string &message)
  {
    if (event_callback_)
    {
      event_callback_(event_type, message);
    }
  }

  bool VideoFileProducer::start()
  {
    ProducerState current_state = state_.load(std::memory_order_acquire);
    if (current_state != ProducerState::STOPPED)
    {
      return false; // Not in stopped state
    }

    SetState(ProducerState::STARTING);
    stop_requested_.store(false, std::memory_order_release);
    teardown_requested_.store(false, std::memory_order_release);
    stub_pts_counter_.store(0, std::memory_order_release);
    next_stub_deadline_utc_.store(0, std::memory_order_release);
    eof_reached_ = false;
    last_pts_us_ = 0;
    last_decoded_frame_pts_us_ = 0;
    first_frame_pts_us_ = 0;
    playback_start_utc_us_ = 0;

    // Set state to RUNNING before starting thread (so loop sees correct state)
    SetState(ProducerState::RUNNING);
    
    // In stub mode, emit ready immediately
    if (config_.stub_mode)
    {
      EmitEvent("ready", "");
    }
    
    // Start producer thread
    producer_thread_ = std::make_unique<std::thread>(&VideoFileProducer::ProduceLoop, this);
    
    std::cout << "[VideoFileProducer] Started for asset: " << config_.asset_uri << std::endl;
    EmitEvent("started", "");
    
    return true;
  }

  void VideoFileProducer::stop()
  {
    ProducerState current_state = state_.load(std::memory_order_acquire);
    if (current_state == ProducerState::STOPPED)
    {
      return; // Already stopped
    }

    SetState(ProducerState::STOPPING);
    stop_requested_.store(true, std::memory_order_release);
    teardown_requested_.store(false, std::memory_order_release);

    // Wait for producer thread to exit
    if (producer_thread_ && producer_thread_->joinable())
    {
      producer_thread_->join();
      producer_thread_.reset();
    }

    CloseDecoder();

    SetState(ProducerState::STOPPED);
    std::cout << "[VideoFileProducer] Stopped. Total decoded frames produced: " 
              << frames_produced_.load(std::memory_order_acquire) << std::endl;
    EmitEvent("stopped", "");
  }

  void VideoFileProducer::RequestTeardown(std::chrono::milliseconds drain_timeout)
  {
    if (!isRunning())
    {
      return;
    }

    drain_timeout_ = drain_timeout;
    teardown_deadline_ = std::chrono::steady_clock::now() + drain_timeout_;
    teardown_requested_.store(true, std::memory_order_release);
    std::cout << "[VideoFileProducer] Teardown requested (timeout="
              << drain_timeout_.count() << " ms)" << std::endl;
    EmitEvent("teardown_requested", "");
  }

  void VideoFileProducer::ForceStop()
  {
    stop_requested_.store(true, std::memory_order_release);
    std::cout << "[VideoFileProducer] Force stop requested" << std::endl;
    EmitEvent("force_stop", "");
  }

  bool VideoFileProducer::isRunning() const
  {
    ProducerState current_state = state_.load(std::memory_order_acquire);
    return current_state == ProducerState::RUNNING;
  }

  uint64_t VideoFileProducer::GetFramesProduced() const
  {
    return frames_produced_.load(std::memory_order_acquire);
  }

  uint64_t VideoFileProducer::GetBufferFullCount() const
  {
    return buffer_full_count_.load(std::memory_order_acquire);
  }

  uint64_t VideoFileProducer::GetDecodeErrors() const
  {
    return decode_errors_.load(std::memory_order_acquire);
  }

  ProducerState VideoFileProducer::GetState() const
  {
    return state_.load(std::memory_order_acquire);
  }

  void VideoFileProducer::ProduceLoop()
  {
    std::cout << "[VideoFileProducer] Decode loop started (stub_mode=" 
              << (config_.stub_mode ? "true" : "false") << ")" << std::endl;

    // Initialize internal decoder if not in stub mode
    if (!config_.stub_mode)
    {
      if (!InitializeDecoder())
      {
        std::cerr << "[VideoFileProducer] Failed to initialize internal decoder, falling back to stub mode" 
                  << std::endl;
        config_.stub_mode = true;  // Fallback to stub mode
        EmitEvent("error", "Failed to initialize internal decoder, falling back to stub mode");
        // Emit ready after fallback to stub mode
        EmitEvent("ready", "");
      }
      else
      {
        std::cout << "[VideoFileProducer] Internal decoder initialized successfully" << std::endl;
        // Emit ready after successful decoder initialization
        EmitEvent("ready", "");
      }
    }

    // Main production loop
    while (!stop_requested_.load(std::memory_order_acquire))
    {
      ProducerState current_state = state_.load(std::memory_order_acquire);
      if (current_state != ProducerState::RUNNING)
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }

      // Check teardown timeout
      if (teardown_requested_.load(std::memory_order_acquire))
      {
        if (output_buffer_.IsEmpty())
        {
          std::cout << "[VideoFileProducer] Buffer drained; completing teardown" << std::endl;
          EmitEvent("buffer_drained", "");
          break;
        }
        if (std::chrono::steady_clock::now() >= teardown_deadline_)
        {
          std::cout << "[VideoFileProducer] Teardown timeout reached; forcing stop" << std::endl;
          EmitEvent("teardown_timeout", "");
          ForceStop();
          break;
        }
      }

      // Check EOF - wait until last frame has been emitted at correct fake time
      if (eof_reached_)
      {
        // If using fake clock, wait until fake time reaches last frame's target UTC time
        if (master_clock_ && master_clock_->is_fake() && 
            last_decoded_frame_pts_us_ > 0 && first_frame_pts_us_ > 0)
        {
          int64_t last_frame_offset_us = last_decoded_frame_pts_us_ - first_frame_pts_us_;
          int64_t last_frame_target_utc_us = playback_start_utc_us_ + last_frame_offset_us;
          int64_t now_us = master_clock_->now_utc_us();
          if (now_us < last_frame_target_utc_us)
          {
            // Wait until fake clock reaches last frame's target UTC time
            while (master_clock_->now_utc_us() < last_frame_target_utc_us &&
                   !stop_requested_.load(std::memory_order_acquire))
            {
              std::this_thread::yield();  // Busy-wait for fake clock
            }
          }
        }
        std::cout << "[VideoFileProducer] End of file reached, all frames emitted" << std::endl;
        EmitEvent("eof", "");
        break;
      }

      if (config_.stub_mode)
      {
        ProduceStubFrame();
        // Small yield to allow other threads
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
      else
      {
        if (!ProduceRealFrame())
        {
          // Decode error or EOF - back off and retry
          if (eof_reached_)
          {
            break;
          }
          // Transient decode error - back off and retry
          decode_errors_.fetch_add(1, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    }

    std::cout << "[VideoFileProducer] Decode loop exited" << std::endl;
    EmitEvent("decode_loop_exited", "");
  }

  bool VideoFileProducer::InitializeDecoder()
  {
#ifdef RETROVUE_FFMPEG_AVAILABLE
    // Allocate format context
    format_ctx_ = avformat_alloc_context();
    if (!format_ctx_)
    {
      std::cerr << "[VideoFileProducer] Failed to allocate format context" << std::endl;
      return false;
    }

    // Open input file
    if (avformat_open_input(&format_ctx_, config_.asset_uri.c_str(), nullptr, nullptr) < 0)
    {
      std::cerr << "[VideoFileProducer] Failed to open input: " << config_.asset_uri << std::endl;
      avformat_free_context(format_ctx_);
      format_ctx_ = nullptr;
      return false;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(format_ctx_, nullptr) < 0)
    {
      std::cerr << "[VideoFileProducer] Failed to find stream info" << std::endl;
      CloseDecoder();
      return false;
    }

    // Find video stream
    video_stream_index_ = -1;
    for (unsigned int i = 0; i < format_ctx_->nb_streams; i++)
    {
      if (format_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      {
        video_stream_index_ = i;
        AVStream* stream = format_ctx_->streams[i];
        time_base_ = av_q2d(stream->time_base);
        break;
      }
    }

    if (video_stream_index_ < 0)
    {
      std::cerr << "[VideoFileProducer] No video stream found" << std::endl;
      CloseDecoder();
      return false;
    }

    // Initialize codec
    AVStream* stream = format_ctx_->streams[video_stream_index_];
    AVCodecParameters* codecpar = stream->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec)
    {
      std::cerr << "[VideoFileProducer] Codec not found: " << codecpar->codec_id << std::endl;
      CloseDecoder();
      return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_)
    {
      std::cerr << "[VideoFileProducer] Failed to allocate codec context" << std::endl;
      CloseDecoder();
      return false;
    }

    if (avcodec_parameters_to_context(codec_ctx_, codecpar) < 0)
    {
      std::cerr << "[VideoFileProducer] Failed to copy codec parameters" << std::endl;
      CloseDecoder();
      return false;
    }

    if (avcodec_open2(codec_ctx_, codec, nullptr) < 0)
    {
      std::cerr << "[VideoFileProducer] Failed to open codec" << std::endl;
      CloseDecoder();
      return false;
    }

    // Allocate frames
    frame_ = av_frame_alloc();
    scaled_frame_ = av_frame_alloc();
    if (!frame_ || !scaled_frame_)
    {
      std::cerr << "[VideoFileProducer] Failed to allocate frames" << std::endl;
      CloseDecoder();
      return false;
    }

    // Initialize scaler
    int src_width = codec_ctx_->width;
    int src_height = codec_ctx_->height;
    AVPixelFormat src_format = codec_ctx_->pix_fmt;
    int dst_width = config_.target_width;
    int dst_height = config_.target_height;
    AVPixelFormat dst_format = AV_PIX_FMT_YUV420P;

    sws_ctx_ = sws_getContext(
        src_width, src_height, src_format,
        dst_width, dst_height, dst_format,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!sws_ctx_)
    {
      std::cerr << "[VideoFileProducer] Failed to create scaler context" << std::endl;
      CloseDecoder();
      return false;
    }

    // Allocate buffer for scaled frame
    if (av_image_alloc(scaled_frame_->data, scaled_frame_->linesize,
                       dst_width, dst_height, dst_format, 32) < 0)
    {
      std::cerr << "[VideoFileProducer] Failed to allocate scaled frame buffer" << std::endl;
      CloseDecoder();
      return false;
    }

    scaled_frame_->width = dst_width;
    scaled_frame_->height = dst_height;
    scaled_frame_->format = dst_format;

    // Allocate packet
    packet_ = av_packet_alloc();
    if (!packet_)
    {
      std::cerr << "[VideoFileProducer] Failed to allocate packet" << std::endl;
      CloseDecoder();
      return false;
    }

    decoder_initialized_ = true;
    eof_reached_ = false;
    return true;
#else
    std::cerr << "[VideoFileProducer] ERROR: FFmpeg not available. Rebuild with FFmpeg to enable real decoding." << std::endl;
    return false;
#endif
  }

  void VideoFileProducer::CloseDecoder()
  {
#ifdef RETROVUE_FFMPEG_AVAILABLE
    if (sws_ctx_)
    {
      sws_freeContext(sws_ctx_);
      sws_ctx_ = nullptr;
    }

    if (scaled_frame_)
    {
      if (scaled_frame_->data[0])
      {
        av_freep(&scaled_frame_->data[0]);
      }
      av_frame_free(&scaled_frame_);
      scaled_frame_ = nullptr;
    }

    if (frame_)
    {
      av_frame_free(&frame_);
      frame_ = nullptr;
    }

    if (packet_)
    {
      av_packet_free(&packet_);
      packet_ = nullptr;
    }

    if (codec_ctx_)
    {
      avcodec_free_context(&codec_ctx_);
      codec_ctx_ = nullptr;
    }

    if (format_ctx_)
    {
      avformat_close_input(&format_ctx_);
      format_ctx_ = nullptr;
    }

    decoder_initialized_ = false;
    video_stream_index_ = -1;
    eof_reached_ = false;
#endif
  }

  bool VideoFileProducer::ProduceRealFrame()
  {
#ifdef RETROVUE_FFMPEG_AVAILABLE
    if (!decoder_initialized_)
    {
      return false;
    }

    // Decode ONE frame at a time (paced according to fake time)
    // Read packet
    int ret = av_read_frame(format_ctx_, packet_);
    
    if (ret == AVERROR_EOF)
    {
      eof_reached_ = true;
      return false;
    }
    
    if (ret < 0)
    {
      av_packet_unref(packet_);
      return false;  // Read error
    }

    // Check if packet is from video stream
    if (packet_->stream_index != video_stream_index_)
    {
      av_packet_unref(packet_);
      return true;  // Skip non-video packets, try again
    }

    // Send packet to decoder
    ret = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    
    if (ret < 0)
    {
      return false;  // Decode error
    }

    // Receive decoded frame
    ret = avcodec_receive_frame(codec_ctx_, frame_);
    
    if (ret == AVERROR(EAGAIN))
    {
      return true;  // Need more packets, try again
    }
    
    if (ret < 0)
    {
      return false;  // Decode error
    }

    // Successfully decoded a frame - scale and assemble
    if (!ScaleFrame())
    {
      return false;
    }

    buffer::Frame output_frame;
    if (!AssembleFrame(output_frame))
    {
      return false;
    }

    // Extract frame PTS in microseconds for pacing
    int64_t frame_pts_us = output_frame.metadata.pts;
    last_decoded_frame_pts_us_ = frame_pts_us;

    // Establish time mapping on first frame
    if (first_frame_pts_us_ == 0)
    {
      first_frame_pts_us_ = frame_pts_us;
      if (master_clock_)
      {
        playback_start_utc_us_ = master_clock_->now_utc_us();
      }
    }

    // Calculate target UTC time for this frame: playback_start + (frame_pts - first_frame_pts)
    int64_t frame_offset_us = frame_pts_us - first_frame_pts_us_;
    int64_t target_utc_us = playback_start_utc_us_ + frame_offset_us;

    // Frame decoded and pushed to buffer

    // If using fake clock, wait until fake time reaches target UTC time before pushing
    if (master_clock_ && master_clock_->is_fake())
    {
      int64_t now_us = master_clock_->now_utc_us();
      if (now_us < target_utc_us)
      {
        // Wait until fake clock reaches target UTC time
        while (master_clock_->now_utc_us() < target_utc_us &&
               !stop_requested_.load(std::memory_order_acquire))
        {
          std::this_thread::yield();  // Busy-wait for fake clock to advance
        }
      }
    }

    // Attempt to push decoded frame
    if (output_buffer_.Push(output_frame))
    {
      frames_produced_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    else
    {
      // Buffer is full, back off
      buffer_full_count_.fetch_add(1, std::memory_order_relaxed);
      if (master_clock_)
      {
        int64_t now_utc_us = master_clock_->now_utc_us();
        int64_t deadline_utc_us = now_utc_us + kProducerBackoffUs;
        if (master_clock_->is_fake())
        {
          // For fake clock, busy-wait
          while (master_clock_->now_utc_us() < deadline_utc_us && 
                 !stop_requested_.load(std::memory_order_acquire))
          {
            std::this_thread::yield();
          }
        }
        else
        {
          while (master_clock_->now_utc_us() < deadline_utc_us && 
                 !stop_requested_.load(std::memory_order_acquire))
          {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
          }
        }
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::microseconds(kProducerBackoffUs));
      }
      // Retry on next iteration
      return true;  // Frame was decoded successfully, just couldn't push
    }
#else
    return false;
#endif
  }

  bool VideoFileProducer::ScaleFrame()
  {
#ifdef RETROVUE_FFMPEG_AVAILABLE
    if (!sws_ctx_ || !frame_ || !scaled_frame_)
    {
      return false;
    }

    sws_scale(sws_ctx_, 
              frame_->data, frame_->linesize, 0, codec_ctx_->height,
              scaled_frame_->data, scaled_frame_->linesize);
    return true;
#else
    return false;
#endif
  }

  bool VideoFileProducer::AssembleFrame(buffer::Frame& output_frame)
  {
#ifdef RETROVUE_FFMPEG_AVAILABLE
    if (!scaled_frame_)
    {
      return false;
    }

    // Set frame dimensions
    output_frame.width = config_.target_width;
    output_frame.height = config_.target_height;

    // Calculate PTS/DTS in microseconds
    // Use frame PTS (from decoded frame) or best_effort_timestamp
    int64_t pts = frame_->pts != AV_NOPTS_VALUE ? frame_->pts : frame_->best_effort_timestamp;
    int64_t dts = frame_->pkt_dts != AV_NOPTS_VALUE ? frame_->pkt_dts : pts;
    
    // Convert to microseconds
    int64_t pts_us = static_cast<int64_t>(pts * time_base_ * kMicrosecondsPerSecond);
    int64_t dts_us = static_cast<int64_t>(dts * time_base_ * kMicrosecondsPerSecond);

    // Ensure PTS monotonicity
    if (pts_us <= last_pts_us_)
    {
      pts_us = last_pts_us_ + frame_interval_us_;
    }
    last_pts_us_ = pts_us;

    // Ensure DTS <= PTS
    if (dts_us > pts_us)
    {
      dts_us = pts_us;
    }

    output_frame.metadata.pts = pts_us;
    output_frame.metadata.dts = dts_us;
    output_frame.metadata.duration = 1.0 / config_.target_fps;
    output_frame.metadata.asset_uri = config_.asset_uri;

    // Copy YUV420 planar data
    int y_size = config_.target_width * config_.target_height;
    int uv_size = (config_.target_width / 2) * (config_.target_height / 2);
    int total_size = y_size + 2 * uv_size;

    output_frame.data.resize(total_size);

    // Copy Y plane
    uint8_t* dst = output_frame.data.data();
    for (int y = 0; y < config_.target_height; y++)
    {
      std::memcpy(dst + y * config_.target_width,
                  scaled_frame_->data[0] + y * scaled_frame_->linesize[0],
                  config_.target_width);
    }

    // Copy U plane
    dst += y_size;
    for (int y = 0; y < config_.target_height / 2; y++)
    {
      std::memcpy(dst + y * (config_.target_width / 2),
                  scaled_frame_->data[1] + y * scaled_frame_->linesize[1],
                  config_.target_width / 2);
    }

    // Copy V plane
    dst += uv_size;
    for (int y = 0; y < config_.target_height / 2; y++)
    {
      std::memcpy(dst + y * (config_.target_width / 2),
                  scaled_frame_->data[2] + y * scaled_frame_->linesize[2],
                  config_.target_width / 2);
    }

    return true;
#else
    return false;
#endif
  }

  void VideoFileProducer::ProduceStubFrame()
  {
    // Wait until deadline (aligned to master clock if available)
    if (master_clock_)
    {
      int64_t now_utc_us = master_clock_->now_utc_us();
      int64_t deadline = next_stub_deadline_utc_.load(std::memory_order_acquire);
      if (deadline == 0)
      {
        // First frame: produce immediately, set next deadline
        deadline = now_utc_us + frame_interval_us_;
        next_stub_deadline_utc_.store(deadline, std::memory_order_release);
        // Don't wait for first frame
      }
      else
      {
        // Wait until deadline for subsequent frames
        while (now_utc_us < deadline && !stop_requested_.load(std::memory_order_acquire))
        {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
          now_utc_us = master_clock_->now_utc_us();
        }
        next_stub_deadline_utc_.store(deadline + frame_interval_us_, std::memory_order_release);
      }
    }
    else
    {
      // Without master clock, check if this is the first frame
      int64_t pts_counter = stub_pts_counter_.load(std::memory_order_acquire);
      if (pts_counter == 0)
      {
        // First frame: produce immediately
      }
      else
      {
        // Subsequent frames: wait for frame interval
        std::this_thread::sleep_for(std::chrono::microseconds(frame_interval_us_));
      }
    }

    // Create stub decoded frame
    buffer::Frame frame;
    frame.width = config_.target_width;
    frame.height = config_.target_height;
    
    int64_t pts_counter = stub_pts_counter_.fetch_add(1, std::memory_order_relaxed);
    frame.metadata.pts = pts_counter * frame_interval_us_;
    frame.metadata.dts = frame.metadata.pts;
    frame.metadata.duration = 1.0 / config_.target_fps;
    frame.metadata.asset_uri = config_.asset_uri;

    // Generate YUV420 planar data (stub: all zeros for now)
    size_t frame_size = static_cast<size_t>(config_.target_width * config_.target_height * 1.5);
    frame.data.resize(frame_size, 0);

    // Attempt to push decoded frame
    if (output_buffer_.Push(frame))
    {
      frames_produced_.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
      // Buffer is full, back off
      buffer_full_count_.fetch_add(1, std::memory_order_relaxed);
      if (master_clock_)
      {
        // Wait using master clock if available
        int64_t now_utc_us = master_clock_->now_utc_us();
        int64_t deadline_utc_us = now_utc_us + kProducerBackoffUs;
        while (master_clock_->now_utc_us() < deadline_utc_us && 
               !stop_requested_.load(std::memory_order_acquire))
        {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::microseconds(kProducerBackoffUs));
      }
    }
  }

} // namespace retrovue::producers::video_file
