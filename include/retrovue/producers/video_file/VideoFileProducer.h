// Repository: Retrovue-playout
// Component: Video File Producer
// Purpose: Decodes local video files and produces frames for the ring buffer.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PRODUCERS_VIDEO_FILE_VIDEO_FILE_PRODUCER_H_
#define RETROVUE_PRODUCERS_VIDEO_FILE_VIDEO_FILE_PRODUCER_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/producers/IProducer.h"

namespace retrovue::timing
{
  class MasterClock;
}

// Forward declarations for FFmpeg types (opaque pointers)
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace retrovue::producers::video_file
{

  // Producer state machine
  enum class ProducerState
  {
    STOPPED,
    STARTING,
    RUNNING,
    STOPPING
  };

  // ProducerConfig holds configuration for video file producer.
  struct ProducerConfig
  {
    std::string asset_uri;       // URI or path to video file
    int target_width;            // Target frame width (e.g., 1920)
    int target_height;           // Target frame height (e.g., 1080)
    double target_fps;           // Target frames per second (e.g., 30.0)
    bool stub_mode;              // If true, generate fake frames instead of decoding
    int tcp_port;                // TCP port for FFmpeg streaming (stub mode)

    ProducerConfig()
        : target_width(1920),
          target_height(1080),
          target_fps(30.0),
          stub_mode(false),
          tcp_port(12345) {}
  };

  // Event callback for producer events (for test harness)
  using ProducerEventCallback = std::function<void(const std::string &event_type, const std::string &message)>;

  // VideoFileProducer is a self-contained decoder that reads video files,
  // decodes them internally using FFmpeg, and produces decoded YUV420 frames.
  //
  // Responsibilities:
  // - Read video files (MP4, MKV, MOV, etc.)
  // - Decode frames internally using libavformat/libavcodec
  // - Scale frames to target resolution
  // - Convert to YUV420 planar format
  // - Push decoded frames to FrameRingBuffer
  // - Handle backpressure and errors gracefully
  //
  // Architecture:
  // - Self-contained: performs both reading and decoding internally
  // - Outputs only decoded frames (never encoded packets)
  // - Internal decoder subsystem: demuxer, decoder, scaler, frame assembly
  class VideoFileProducer : public retrovue::producers::IProducer
  {
  public:
    // Constructs a producer with the given configuration and output buffer.
    VideoFileProducer(
        const ProducerConfig &config,
        buffer::FrameRingBuffer &output_buffer,
        std::shared_ptr<timing::MasterClock> clock = nullptr,
        ProducerEventCallback event_callback = nullptr);

    ~VideoFileProducer();

    // Disable copy and move
    VideoFileProducer(const VideoFileProducer &) = delete;
    VideoFileProducer &operator=(const VideoFileProducer &) = delete;

    // IProducer interface
    bool start() override;
    void stop() override;
    bool isRunning() const override;

    // Initiates graceful teardown with bounded drain timeout.
    void RequestTeardown(std::chrono::milliseconds drain_timeout);

    // Forces immediate stop (used when teardown times out).
    void ForceStop();

    // Returns the total number of decoded frames produced.
    uint64_t GetFramesProduced() const;

    // Returns the number of times the buffer was full (backpressure events).
    uint64_t GetBufferFullCount() const;

    // Returns the number of decode errors encountered.
    uint64_t GetDecodeErrors() const;

    // Returns current producer state.
    ProducerState GetState() const;

  private:
    // Main production loop (runs in producer thread).
    void ProduceLoop();

    // Stub implementation: generates synthetic decoded frames (for testing).
    void ProduceStubFrame();

    // Real decode implementation: reads, decodes, scales, and assembles frames.
    bool ProduceRealFrame();

    // Internal decoder subsystem initialization.
    bool InitializeDecoder();
    void CloseDecoder();

    // Internal decoder operations.
    bool ReadPacket();
    bool DecodePacket();
    bool ScaleFrame();
    bool AssembleFrame(buffer::Frame& frame);

    // Emits producer event through callback.
    void EmitEvent(const std::string &event_type, const std::string &message = "");

    // Transitions state (thread-safe).
    void SetState(ProducerState new_state);

    ProducerConfig config_;
    buffer::FrameRingBuffer &output_buffer_;
    std::shared_ptr<timing::MasterClock> master_clock_;
    ProducerEventCallback event_callback_;

    std::atomic<ProducerState> state_;
    std::atomic<bool> stop_requested_;
    std::atomic<bool> teardown_requested_;
    std::atomic<uint64_t> frames_produced_;
    std::atomic<uint64_t> buffer_full_count_;
    std::atomic<uint64_t> decode_errors_;
    std::chrono::steady_clock::time_point teardown_deadline_;
    std::chrono::milliseconds drain_timeout_;

    std::unique_ptr<std::thread> producer_thread_;

    // Internal decoder subsystem (FFmpeg)
    AVFormatContext* format_ctx_;
    AVCodecContext* codec_ctx_;
    AVFrame* frame_;
    AVFrame* scaled_frame_;
    AVPacket* packet_;
    SwsContext* sws_ctx_;
    int video_stream_index_;
    bool decoder_initialized_;
    bool eof_reached_;
    double time_base_;  // Stream time base for PTS/DTS conversion
    int64_t last_pts_us_;  // For PTS monotonicity enforcement
    int64_t last_decoded_frame_pts_us_;  // PTS of last decoded frame (for EOF pacing)
    int64_t first_frame_pts_us_;  // PTS of first frame (for establishing time mapping)
    int64_t playback_start_utc_us_;  // UTC time when first frame was decoded (for pacing)

    // State for stub frame generation
    std::atomic<int64_t> stub_pts_counter_;
    int64_t frame_interval_us_;
    std::atomic<int64_t> next_stub_deadline_utc_;
  };

} // namespace retrovue::producers::video_file

#endif // RETROVUE_PRODUCERS_VIDEO_FILE_VIDEO_FILE_PRODUCER_H_
