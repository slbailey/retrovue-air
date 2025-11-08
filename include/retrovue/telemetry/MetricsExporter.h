// Repository: Retrovue-playout
// Component: Metrics Exporter
// Purpose: Exposes Prometheus metrics at /metrics HTTP endpoint.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_TELEMETRY_METRICS_EXPORTER_H_
#define RETROVUE_TELEMETRY_METRICS_EXPORTER_H_

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace retrovue::telemetry {

// Forward declaration
class MetricsHTTPServer;

// ChannelState represents the current state of a playout channel.
enum class ChannelState {
  STOPPED = 0,
  BUFFERING = 1,
  READY = 2,
  ERROR_STATE = 3
};

// Convert ChannelState to string for metrics output.
const char* ChannelStateToString(ChannelState state);

// ChannelMetrics holds per-channel telemetry data.
struct ChannelMetrics {
  ChannelState state;
  uint64_t buffer_depth_frames;
  double frame_gap_seconds;
  uint64_t decode_failure_count;
  
  ChannelMetrics()
      : state(ChannelState::STOPPED),
        buffer_depth_frames(0),
        frame_gap_seconds(0.0),
        decode_failure_count(0) {}
};

// MetricsExporter serves Prometheus metrics at an HTTP endpoint.
//
// Phase 2 Implementation:
// - Simple HTTP server serving /metrics endpoint
// - Text-based Prometheus exposition format
// - Thread-safe metric updates
//
// Metrics Exported:
// - retrovue_playout_channel_state{channel="N"} - gauge
// - retrovue_playout_buffer_depth_frames{channel="N"} - gauge
// - retrovue_playout_frame_gap_seconds{channel="N"} - gauge
// - retrovue_playout_decode_failure_count{channel="N"} - counter
//
// Usage:
// 1. Construct with port number
// 2. Call Start() to begin serving metrics
// 3. Update metrics using UpdateChannelMetrics()
// 4. Call Stop() to shutdown server
class MetricsExporter {
 public:
  // Constructs an exporter that will serve on the specified port.
  explicit MetricsExporter(int port = 9308);
  
  ~MetricsExporter();

  // Disable copy and move
  MetricsExporter(const MetricsExporter&) = delete;
  MetricsExporter& operator=(const MetricsExporter&) = delete;

  // Starts the metrics HTTP server.
  // Returns true if started successfully.
  bool Start();

  // Stops the metrics HTTP server.
  void Stop();

  // Returns true if the exporter is currently running.
  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  // Updates metrics for a specific channel.
  void UpdateChannelMetrics(int32_t channel_id, const ChannelMetrics& metrics);

  // Removes metrics for a channel (when channel stops).
  void RemoveChannel(int32_t channel_id);

  // Gets the current metrics for a channel.
  // Returns false if channel doesn't exist.
  bool GetChannelMetrics(int32_t channel_id, ChannelMetrics& metrics) const;

 private:
  // Generates Prometheus-format metrics text.
  std::string GenerateMetricsText() const;

  int port_;
  std::atomic<bool> running_;
  std::atomic<bool> stop_requested_;
  
  std::unique_ptr<MetricsHTTPServer> http_server_;
  
  // Channel metrics storage (protected by mutex)
  mutable std::mutex metrics_mutex_;
  std::map<int32_t, ChannelMetrics> channel_metrics_;
};

}  // namespace retrovue::telemetry

#endif  // RETROVUE_TELEMETRY_METRICS_EXPORTER_H_

