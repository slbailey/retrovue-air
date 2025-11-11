// Repository: Retrovue-playout
// Component: Frame Renderer
// Purpose: Render decoded frames to display or headless output.
// Copyright (c) 2025 RetroVue

#include "retrovue/renderer/FrameRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef RETROVUE_SDL2_AVAILABLE
extern "C" {
#include <SDL2/SDL.h>
}
#endif

#include "retrovue/telemetry/MetricsExporter.h"
#include "retrovue/timing/MasterClock.h"

namespace retrovue::renderer {

namespace {
constexpr double kWaitFudgeSeconds = 0.001;                // wake a millisecond early
constexpr double kDropThresholdSeconds = -0.008;           // drop when we are 8 ms late (MC-003)
constexpr int kMinDepthForDrop = 5;                        // keep buffer from starving
constexpr int64_t kSpinThresholdUs = 200;                  // busy wait for last 0.2 ms
constexpr int64_t kSpinSleepUs = 100;                      // fine-grained wait window
constexpr int64_t kEmptyBufferBackoffUs = 5'000;           // MC-004: allow producer to refill
constexpr int64_t kErrorBackoffUs = 10'000;                // MC-004 recovery assistance

inline void WaitUntilUtc(const std::shared_ptr<timing::MasterClock>& clock,
                         int64_t target_utc_us) {
  if (!clock || target_utc_us <= 0) {
    return;
  }

  // MC-003: Align renderer pacing with MasterClock monotonic schedule.
  while (true) {
    const int64_t now = clock->now_utc_us();
    const int64_t remaining = target_utc_us - now;
    if (remaining <= 0) {
      break;
    }

    const int64_t sleep_us =
        (remaining > 2'000) ? remaining - 1'000
                            : std::max<int64_t>(remaining / 2, 200);
    std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
  }
}

inline void WaitForMicros(const std::shared_ptr<timing::MasterClock>& clock,
                          int64_t duration_us) {
  if (duration_us <= 0) {
    return;
  }
  if (clock) {
    WaitUntilUtc(clock, clock->now_utc_us() + duration_us);
    return;
  }
  std::this_thread::sleep_for(std::chrono::microseconds(duration_us));
}

inline int64_t WaitFudgeUs() {
  return static_cast<int64_t>(kWaitFudgeSeconds * 1'000'000.0);
}
}  // namespace

FrameRenderer::FrameRenderer(const RenderConfig& config,
                             buffer::FrameRingBuffer& input_buffer,
                             const std::shared_ptr<timing::MasterClock>& clock,
                             const std::shared_ptr<telemetry::MetricsExporter>& metrics,
                             int32_t channel_id)
    : config_(config),
      input_buffer_(input_buffer),
      clock_(clock),
      metrics_(metrics),
      channel_id_(channel_id),
      running_(false),
      stop_requested_(false),
      last_pts_(0),
      last_frame_time_utc_(0),
      fallback_last_frame_time_(std::chrono::steady_clock::now()) {}

FrameRenderer::~FrameRenderer() { Stop(); }

std::unique_ptr<FrameRenderer> FrameRenderer::Create(
    const RenderConfig& config, buffer::FrameRingBuffer& input_buffer,
    const std::shared_ptr<timing::MasterClock>& clock,
    const std::shared_ptr<telemetry::MetricsExporter>& metrics,
    int32_t channel_id) {
  if (config.mode == RenderMode::PREVIEW) {
#ifdef RETROVUE_SDL2_AVAILABLE
    return std::make_unique<PreviewRenderer>(config, input_buffer, clock, metrics,
                                             channel_id);
#else
    std::cerr << "[FrameRenderer] WARNING: SDL2 not available, using headless mode"
              << std::endl;
    return std::make_unique<HeadlessRenderer>(config, input_buffer, clock, metrics,
                                              channel_id);
#endif
  }

  return std::make_unique<HeadlessRenderer>(config, input_buffer, clock, metrics,
                                            channel_id);
}

bool FrameRenderer::Start() {
  if (running_.load(std::memory_order_acquire)) {
    std::cerr << "[FrameRenderer] Already running" << std::endl;
    return false;
  }

  stop_requested_.store(false, std::memory_order_release);
  
  render_thread_ = std::make_unique<std::thread>(&FrameRenderer::RenderLoop, this);
  
  std::cout << "[FrameRenderer] Started" << std::endl;
  return true;
}

void FrameRenderer::Stop() {
  if (!running_.load(std::memory_order_acquire) && !render_thread_) {
    return;
  }

  std::cout << "[FrameRenderer] Stopping..." << std::endl;
  stop_requested_.store(true, std::memory_order_release);

  if (render_thread_ && render_thread_->joinable()) {
    render_thread_->join();
  }

  render_thread_.reset();
  running_.store(false, std::memory_order_release);
  
  std::cout << "[FrameRenderer] Stopped. Total frames rendered: " 
            << stats_.frames_rendered << std::endl;
}

void FrameRenderer::RenderLoop() {
  std::cout << "[FrameRenderer] Render loop started (mode=" 
            << (config_.mode == RenderMode::HEADLESS ? "HEADLESS" : "PREVIEW") 
            << ")" << std::endl;

  // Initialize renderer
  if (!Initialize()) {
    std::cerr << "[FrameRenderer] Failed to initialize" << std::endl;
    return;
  }

  running_.store(true, std::memory_order_release);
  if (clock_) {
    last_frame_time_utc_ = clock_->now_utc_us();
  } else {
    fallback_last_frame_time_ = std::chrono::steady_clock::now();
  }

  while (!stop_requested_.load(std::memory_order_acquire)) {
    int64_t frame_start_utc = 0;
    std::chrono::steady_clock::time_point frame_start_fallback;
    if (clock_) {
      frame_start_utc = clock_->now_utc_us();
    } else {
      frame_start_fallback = std::chrono::steady_clock::now();
    }

    // Try to pop a frame from the buffer
    buffer::Frame frame;
    if (!input_buffer_.Pop(frame)) {
      WaitForMicros(clock_, kEmptyBufferBackoffUs);  // MC-004: allow producer to refill
      stats_.frames_skipped++;
      continue;
    }

    double frame_gap_ms = 0.0;
    if (clock_) {
      const int64_t deadline_utc = clock_->scheduled_to_utc_us(frame.metadata.pts);
      const int64_t now_utc = clock_->now_utc_us();
      const int64_t gap_us = deadline_utc - now_utc;
      const double gap_s = static_cast<double>(gap_us) / 1'000'000.0;
      frame_gap_ms = gap_s * 1000.0;

      if (gap_s > 0.0) {
        const int64_t deadline_utc =
            clock_->scheduled_to_utc_us(frame.metadata.pts);
        const int64_t target_utc = deadline_utc - WaitFudgeUs();
        WaitUntilUtc(clock_, target_utc);  // MC-003: pace rendering to MasterClock

        int64_t remaining_us =
            clock_->scheduled_to_utc_us(frame.metadata.pts) - clock_->now_utc_us();
        while (remaining_us > kSpinThresholdUs) {
          const int64_t spin_us =
              std::min<int64_t>(remaining_us / 2, kSpinSleepUs);
          WaitForMicros(clock_, spin_us);
          remaining_us =
              clock_->scheduled_to_utc_us(frame.metadata.pts) - clock_->now_utc_us();
        }
      } else if (gap_s < kDropThresholdSeconds &&
                 input_buffer_.Size() > kMinDepthForDrop) {
        stats_.frames_dropped++;
        stats_.corrections_total++;
        PublishMetrics(frame_gap_ms);
        continue;
      }
    } else {
      auto now = std::chrono::steady_clock::now();
      frame_gap_ms =
          std::chrono::duration<double, std::milli>(now - fallback_last_frame_time_).count();
      fallback_last_frame_time_ = now;
    }

    RenderFrame(frame);

    int64_t frame_end_utc = 0;
    std::chrono::steady_clock::time_point frame_end_fallback;
    if (clock_) {
      frame_end_utc = clock_->now_utc_us();
    } else {
      frame_end_fallback = std::chrono::steady_clock::now();
    }

    double render_time_ms = 0.0;
    if (clock_) {
      render_time_ms =
          static_cast<double>(frame_end_utc - frame_start_utc) / 1'000.0;
      last_frame_time_utc_ = frame_end_utc;
    } else {
      render_time_ms =
          std::chrono::duration<double, std::milli>(frame_end_fallback - frame_start_fallback)
              .count();
      fallback_last_frame_time_ = frame_end_fallback;
    }

    UpdateStats(render_time_ms, frame_gap_ms);
    PublishMetrics(frame_gap_ms);

    // Log progress periodically
    if (stats_.frames_rendered % 100 == 0) {
      std::cout << "[FrameRenderer] Rendered " << stats_.frames_rendered 
                << " frames, avg render time: " << stats_.average_render_time_ms << "ms, "
                << "fps: " << stats_.current_render_fps 
                << ", gap: " << frame_gap_ms << "ms" << std::endl;
    }

    last_pts_ = frame.metadata.pts;
  }

  // Cleanup renderer
  Cleanup();
  running_.store(false, std::memory_order_release);
  
  std::cout << "[FrameRenderer] Render loop exited" << std::endl;
}

void FrameRenderer::UpdateStats(double render_time_ms, double frame_gap_ms) {
  stats_.frames_rendered++;
  stats_.frame_gap_ms = frame_gap_ms;

  // Update average render time (exponential moving average)
  const double alpha = 0.1;
  stats_.average_render_time_ms = 
      alpha * render_time_ms + (1.0 - alpha) * stats_.average_render_time_ms;

  // Calculate current render FPS
  if (frame_gap_ms > 0.0) {
    stats_.current_render_fps = 1000.0 / frame_gap_ms;
  }
}

void FrameRenderer::PublishMetrics(double frame_gap_ms) {
  if (!metrics_) {
    return;
  }

  telemetry::ChannelMetrics snapshot;
  if (!metrics_->GetChannelMetrics(channel_id_, snapshot)) {
    snapshot = telemetry::ChannelMetrics{};
  }

  snapshot.buffer_depth_frames = input_buffer_.Size();
  snapshot.frame_gap_seconds = frame_gap_ms / 1000.0;
  snapshot.corrections_total = stats_.corrections_total;
  metrics_->UpdateChannelMetrics(channel_id_, snapshot);
}

// ============================================================================
// HeadlessRenderer
// ============================================================================

HeadlessRenderer::HeadlessRenderer(const RenderConfig& config,
                                   buffer::FrameRingBuffer& input_buffer,
                                   const std::shared_ptr<timing::MasterClock>& clock,
                                   const std::shared_ptr<telemetry::MetricsExporter>& metrics,
                                   int32_t channel_id)
    : FrameRenderer(config, input_buffer, clock, metrics, channel_id) {}

HeadlessRenderer::~HeadlessRenderer() {
}

bool HeadlessRenderer::Initialize() {
  std::cout << "[HeadlessRenderer] Initialized (no display output)" << std::endl;
  return true;
}

void HeadlessRenderer::RenderFrame(const buffer::Frame& frame) {
  // Headless mode: just consume the frame without displaying
  // This validates frame timing and buffer consumption
  // In production, this might feed into SDI output or network stream
  
  // Simulate minimal processing time
  // Real implementation would push to hardware output
}

void HeadlessRenderer::Cleanup() {
  std::cout << "[HeadlessRenderer] Cleanup complete" << std::endl;
}

// ============================================================================
// PreviewRenderer
// ============================================================================

#ifdef RETROVUE_SDL2_AVAILABLE

PreviewRenderer::PreviewRenderer(const RenderConfig& config,
                                 buffer::FrameRingBuffer& input_buffer,
                                 const std::shared_ptr<timing::MasterClock>& clock,
                                 const std::shared_ptr<telemetry::MetricsExporter>& metrics,
                                 int32_t channel_id)
    : FrameRenderer(config, input_buffer, clock, metrics, channel_id),
      window_(nullptr),
      renderer_(nullptr),
      texture_(nullptr) {
}

PreviewRenderer::~PreviewRenderer() {
}

bool PreviewRenderer::Initialize() {
  std::cout << "[PreviewRenderer] Initializing SDL2..." << std::endl;

  if (SDL_Init(SDL_INIT_VIDEO) < 0) {
    std::cerr << "[PreviewRenderer] SDL_Init failed: " << SDL_GetError() << std::endl;
    return false;
  }

  // Create window
  SDL_Window* window = SDL_CreateWindow(
      config_.window_title.c_str(),
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      config_.window_width,
      config_.window_height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);

  if (!window) {
    std::cerr << "[PreviewRenderer] SDL_CreateWindow failed: " << SDL_GetError() 
              << std::endl;
    SDL_Quit();
    return false;
  }
  window_ = window;

  // Create renderer
  Uint32 flags = SDL_RENDERER_ACCELERATED;
  if (config_.vsync_enabled) {
    flags |= SDL_RENDERER_PRESENTVSYNC;
  }

  SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, flags);
  if (!renderer) {
    std::cerr << "[PreviewRenderer] SDL_CreateRenderer failed: " << SDL_GetError() 
              << std::endl;
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }
  renderer_ = renderer;

  // Create texture for YUV420 frames
  SDL_Texture* texture = SDL_CreateTexture(
      renderer,
      SDL_PIXELFORMAT_IYUV,  // YUV420P
      SDL_TEXTUREACCESS_STREAMING,
      config_.window_width,
      config_.window_height);

  if (!texture) {
    std::cerr << "[PreviewRenderer] SDL_CreateTexture failed: " << SDL_GetError() 
              << std::endl;
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return false;
  }
  texture_ = texture;

  std::cout << "[PreviewRenderer] Initialized successfully: " 
            << config_.window_width << "x" << config_.window_height << std::endl;

  return true;
}

void PreviewRenderer::RenderFrame(const buffer::Frame& frame) {
  SDL_Window* window = static_cast<SDL_Window*>(window_);
  SDL_Renderer* renderer = static_cast<SDL_Renderer*>(renderer_);
  SDL_Texture* texture = static_cast<SDL_Texture*>(texture_);

  // Handle SDL events (window close, etc.)
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      stop_requested_.store(true, std::memory_order_release);
      return;
    }
  }

  // Update texture with YUV420 data
  if (!frame.data.empty()) {
    int y_size = frame.width * frame.height;
    int uv_size = (frame.width / 2) * (frame.height / 2);

    const uint8_t* y_plane = frame.data.data();
    const uint8_t* u_plane = y_plane + y_size;
    const uint8_t* v_plane = u_plane + uv_size;

    SDL_UpdateYUVTexture(
        texture,
        nullptr,
        y_plane, frame.width,
        u_plane, frame.width / 2,
        v_plane, frame.width / 2);
  }

  // Render texture to window
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  SDL_RenderPresent(renderer);
}

void PreviewRenderer::Cleanup() {
  std::cout << "[PreviewRenderer] Cleaning up SDL2..." << std::endl;

  if (texture_) {
    SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
    texture_ = nullptr;
  }

  if (renderer_) {
    SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));
    renderer_ = nullptr;
  }

  if (window_) {
    SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
    window_ = nullptr;
  }

  SDL_Quit();
  std::cout << "[PreviewRenderer] Cleanup complete" << std::endl;
}

#else
// Stub implementations when SDL2 not available

PreviewRenderer::PreviewRenderer(const RenderConfig& config,
                                 buffer::FrameRingBuffer& input_buffer,
                                 const std::shared_ptr<timing::MasterClock>& clock,
                                 const std::shared_ptr<telemetry::MetricsExporter>& metrics,
                                 int32_t channel_id)
    : FrameRenderer(config, input_buffer, clock, metrics, channel_id),
      window_(nullptr),
      renderer_(nullptr),
      texture_(nullptr) {
}

PreviewRenderer::~PreviewRenderer() {
}

bool PreviewRenderer::Initialize() {
  std::cerr << "[PreviewRenderer] ERROR: SDL2 not available. Rebuild with SDL2 for preview mode." 
            << std::endl;
  return false;
}

void PreviewRenderer::RenderFrame(const buffer::Frame& frame) {
}

void PreviewRenderer::Cleanup() {
}

#endif  // RETROVUE_SDL2_AVAILABLE

}  // namespace retrovue::renderer

