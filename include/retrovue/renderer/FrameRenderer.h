// Repository: Retrovue-playout
// Component: Frame Renderer
// Purpose: Render decoded frames to display or headless output.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_RENDERER_FRAME_RENDERER_H_
#define RETROVUE_RENDERER_FRAME_RENDERER_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "retrovue/buffer/FrameRingBuffer.h"

namespace retrovue::renderer {

// RenderMode specifies the rendering output type.
enum class RenderMode {
  HEADLESS = 0,  // No display output (production mode)
  PREVIEW = 1,   // Preview window (debug/development mode)
};

// RenderConfig holds configuration for the renderer.
struct RenderConfig {
  RenderMode mode;
  int window_width;
  int window_height;
  std::string window_title;
  bool vsync_enabled;
  
  RenderConfig()
      : mode(RenderMode::HEADLESS),
        window_width(1920),
        window_height(1080),
        window_title("RetroVue Playout Preview"),
        vsync_enabled(true) {}
};

// RenderStats tracks rendering performance and frame timing.
struct RenderStats {
  uint64_t frames_rendered;
  uint64_t frames_skipped;
  uint64_t frames_dropped;
  double average_render_time_ms;
  double current_render_fps;
  double frame_gap_ms;  // Time since last frame
  
  RenderStats()
      : frames_rendered(0),
        frames_skipped(0),
        frames_dropped(0),
        average_render_time_ms(0.0),
        current_render_fps(0.0),
        frame_gap_ms(0.0) {}
};

// FrameRenderer consumes frames from the ring buffer and renders them.
//
// Design:
// - Abstract base class with two concrete implementations:
//   - HeadlessRenderer: Consumes frames without display (production)
//   - PreviewRenderer: Opens SDL2/OpenGL window (debug/development)
// - Runs in dedicated render thread
// - Frame timing driven by metadata.pts
// - Back-pressure handling when buffer empty
//
// Thread Model:
// - Renderer runs in its own thread
// - Pops frames from FrameRingBuffer (thread-safe)
// - Independent from decode thread
//
// Lifecycle:
// 1. Construct with config and ring buffer reference
// 2. Call Start() to begin rendering
// 3. Call Stop() to gracefully shutdown
// 4. Destructor ensures thread is joined
class FrameRenderer {
 public:
  virtual ~FrameRenderer();

  // Starts the render thread.
  // Returns true if started successfully.
  bool Start();

  // Stops the render thread gracefully.
  void Stop();

  // Returns true if renderer is currently running.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Gets current render statistics.
  const RenderStats& GetStats() const { return stats_; }

  // Factory method to create appropriate renderer based on mode.
  static std::unique_ptr<FrameRenderer> Create(
      const RenderConfig& config,
      buffer::FrameRingBuffer& input_buffer);

 protected:
  // Protected constructor - use factory method.
  FrameRenderer(const RenderConfig& config, buffer::FrameRingBuffer& input_buffer);

  // Main render loop (runs in render thread).
  void RenderLoop();

  // Subclass-specific initialization.
  // Called once before render loop starts.
  virtual bool Initialize() = 0;

  // Subclass-specific frame rendering.
  // Called for each frame in the render loop.
  virtual void RenderFrame(const buffer::Frame& frame) = 0;

  // Subclass-specific cleanup.
  // Called once after render loop exits.
  virtual void Cleanup() = 0;

  // Updates renderer statistics.
  void UpdateStats(double render_time_ms, double frame_gap_ms);

  RenderConfig config_;
  buffer::FrameRingBuffer& input_buffer_;
  RenderStats stats_;

  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  std::unique_ptr<std::thread> render_thread_;
  
  int64_t last_pts_;
  std::chrono::steady_clock::time_point last_frame_time_;
};

// HeadlessRenderer consumes frames without displaying them.
// Used in production environments where no display is available.
class HeadlessRenderer : public FrameRenderer {
 public:
  HeadlessRenderer(const RenderConfig& config, buffer::FrameRingBuffer& input_buffer);
  ~HeadlessRenderer() override;

 protected:
  bool Initialize() override;
  void RenderFrame(const buffer::Frame& frame) override;
  void Cleanup() override;
};

// PreviewRenderer displays frames in an SDL2 window.
// Used for development and debugging.
class PreviewRenderer : public FrameRenderer {
 public:
  PreviewRenderer(const RenderConfig& config, buffer::FrameRingBuffer& input_buffer);
  ~PreviewRenderer() override;

 protected:
  bool Initialize() override;
  void RenderFrame(const buffer::Frame& frame) override;
  void Cleanup() override;

 private:
  // SDL2/OpenGL context (opaque pointers)
  void* window_;      // SDL_Window*
  void* renderer_;    // SDL_Renderer*
  void* texture_;     // SDL_Texture*
};

}  // namespace retrovue::renderer

#endif  // RETROVUE_RENDERER_FRAME_RENDERER_H_

