// Repository: Retrovue-playout
// Component: Metrics Exporter
// Purpose: Exposes Prometheus metrics at /metrics HTTP endpoint.
// Copyright (c) 2025 RetroVue

#include "retrovue/telemetry/MetricsExporter.h"
#include "retrovue/telemetry/MetricsHTTPServer.h"

#include <iostream>
#include <sstream>
#include <thread>

namespace retrovue::telemetry {

const char* ChannelStateToString(ChannelState state) {
  switch (state) {
    case ChannelState::STOPPED:
      return "stopped";
    case ChannelState::BUFFERING:
      return "buffering";
    case ChannelState::READY:
      return "ready";
    case ChannelState::ERROR_STATE:
      return "error";
    default:
      return "unknown";
  }
}

int ChannelStateToValue(ChannelState state) {
  return static_cast<int>(state);
}

MetricsExporter::MetricsExporter(int port)
    : port_(port),
      running_(false),
      stop_requested_(false),
      http_server_(std::make_unique<MetricsHTTPServer>(port)) {
  
  // Set up metrics callback
  http_server_->SetMetricsCallback([this]() {
    return this->GenerateMetricsText();
  });
}

MetricsExporter::~MetricsExporter() {
  Stop();
}

bool MetricsExporter::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return false;  // Already running
  }

  // Start HTTP server
  if (!http_server_->Start()) {
    std::cerr << "[MetricsExporter] Failed to start HTTP server" << std::endl;
    running_.store(false, std::memory_order_release);
    return false;
  }

  std::cout << "[MetricsExporter] Started HTTP server on port " << port_ 
            << " - metrics at http://localhost:" << port_ << "/metrics" << std::endl;
  return true;
}

void MetricsExporter::Stop() {
  if (!running_.load(std::memory_order_acquire)) {
    return;  // Not running
  }

  std::cout << "[MetricsExporter] Stopping..." << std::endl;
  
  // Stop HTTP server
  http_server_->Stop();

  running_.store(false, std::memory_order_release);
  std::cout << "[MetricsExporter] Stopped" << std::endl;
}

void MetricsExporter::UpdateChannelMetrics(int32_t channel_id, 
                                            const ChannelMetrics& metrics) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  channel_metrics_[channel_id] = metrics;
}

void MetricsExporter::RemoveChannel(int32_t channel_id) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  channel_metrics_.erase(channel_id);
}

bool MetricsExporter::GetChannelMetrics(int32_t channel_id, 
                                         ChannelMetrics& metrics) const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  auto it = channel_metrics_.find(channel_id);
  if (it == channel_metrics_.end()) {
    return false;
  }
  metrics = it->second;
  return true;
}

std::string MetricsExporter::GenerateMetricsText() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  
  if (channel_metrics_.empty()) {
    return "";
  }
  
  std::ostringstream oss;
  
  // Header comments
  oss << "# HELP retrovue_playout_channel_state Current state of playout channel\n";
  oss << "# TYPE retrovue_playout_channel_state gauge\n";
  
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_channel_state{channel=\"" << channel_id 
        << "\",state=\"" << ChannelStateToString(metrics.state) << "\"} " 
        << ChannelStateToValue(metrics.state) << "\n";
  }
  
  oss << "\n# HELP retrovue_playout_buffer_depth_frames Number of frames in buffer\n";
  oss << "# TYPE retrovue_playout_buffer_depth_frames gauge\n";
  
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_buffer_depth_frames{channel=\"" << channel_id 
        << "\"} " << metrics.buffer_depth_frames << "\n";
  }
  
  oss << "\n# HELP retrovue_playout_frame_gap_seconds Timing deviation from MasterClock\n";
  oss << "# TYPE retrovue_playout_frame_gap_seconds gauge\n";
  
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_frame_gap_seconds{channel=\"" << channel_id 
        << "\"} " << metrics.frame_gap_seconds << "\n";
  }
  
  oss << "\n# HELP retrovue_playout_decode_failure_count Total decode failures\n";
  oss << "# TYPE retrovue_playout_decode_failure_count counter\n";
  
  for (const auto& [channel_id, metrics] : channel_metrics_) {
    oss << "retrovue_playout_decode_failure_count{channel=\"" << channel_id 
        << "\"} " << metrics.decode_failure_count << "\n";
  }
  
  return oss.str();
}

}  // namespace retrovue::telemetry

