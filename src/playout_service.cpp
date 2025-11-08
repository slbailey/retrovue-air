// Repository: Retrovue-playout
// Component: PlayoutControl gRPC Service Implementation
// Purpose: Implements the PlayoutControl service interface for channel lifecycle management.
// Copyright (c) 2025 RetroVue

#include "playout_service.h"

#include <iostream>
#include <string>

namespace retrovue {
namespace playout {

namespace {
constexpr char kApiVersion[] = "1.0.0";
constexpr size_t kDefaultBufferSize = 60;  // 60 frames (~2 seconds at 30fps)
}  // namespace

PlayoutControlImpl::PlayoutControlImpl(
    std::shared_ptr<telemetry::MetricsExporter> metrics_exporter)
    : metrics_exporter_(metrics_exporter) {
  std::cout << "[PlayoutControlImpl] Service initialized (API version: " << kApiVersion << ")"
            << std::endl;
}

PlayoutControlImpl::~PlayoutControlImpl() {
  std::cout << "[PlayoutControlImpl] Service shutting down" << std::endl;
  
  // Stop all active channels
  std::lock_guard<std::mutex> lock(channels_mutex_);
  for (auto& [channel_id, worker] : active_channels_) {
    std::cout << "[PlayoutControlImpl] Stopping channel " << channel_id << std::endl;
    if (worker->renderer) {
      worker->renderer->Stop();
    }
    if (worker->producer) {
      worker->producer->Stop();
    }
    if (metrics_exporter_) {
      metrics_exporter_->RemoveChannel(channel_id);
    }
  }
  active_channels_.clear();
}

grpc::Status PlayoutControlImpl::StartChannel(grpc::ServerContext* context,
                                               const StartChannelRequest* request,
                                               StartChannelResponse* response) {
  std::lock_guard<std::mutex> lock(channels_mutex_);

  const int32_t channel_id = request->channel_id();
  const std::string& plan_handle = request->plan_handle();
  const int32_t port = request->port();

  std::cout << "[StartChannel] Request received: channel_id=" << channel_id
            << ", plan_handle=" << plan_handle << ", port=" << port << std::endl;

  // Check if channel is already active
  if (active_channels_.find(channel_id) != active_channels_.end()) {
    response->set_success(false);
    response->set_message("Channel already active");
    return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                        "Channel is already running");
  }

  // Create channel worker
  auto worker = std::make_unique<ChannelWorker>(channel_id, plan_handle, port);

  // Initialize ring buffer
  worker->ring_buffer = std::make_unique<buffer::FrameRingBuffer>(kDefaultBufferSize);

  // Configure frame producer
  decode::ProducerConfig producer_config;
  producer_config.asset_uri = plan_handle;  // Use plan_handle as asset URI for now
  producer_config.target_width = 1920;
  producer_config.target_height = 1080;
  producer_config.target_fps = 30.0;
  producer_config.stub_mode = false;  // Phase 3: real decode (falls back to stub if FFmpeg unavailable)

  // Create producer
  worker->producer = std::make_unique<decode::FrameProducer>(
      producer_config, *worker->ring_buffer);

  // Start decode thread
  if (!worker->producer->Start()) {
    response->set_success(false);
    response->set_message("Failed to start frame producer");
    return grpc::Status(grpc::StatusCode::INTERNAL, "Producer start failed");
  }

  // Configure and create renderer (Phase 3)
  renderer::RenderConfig render_config;
  render_config.mode = renderer::RenderMode::HEADLESS;  // Default to headless mode
  render_config.window_width = 1920;
  render_config.window_height = 1080;
  render_config.window_title = "RetroVue Channel " + std::to_string(channel_id);

  worker->renderer = renderer::FrameRenderer::Create(render_config, *worker->ring_buffer);

  // Start render thread
  if (!worker->renderer->Start()) {
    std::cerr << "[StartChannel] WARNING: Failed to start renderer, continuing without it" 
              << std::endl;
    // Don't fail the entire StartChannel operation if renderer fails
    // Producer will fill buffer, it just won't be consumed
  }

  // Update metrics
  if (metrics_exporter_) {
    telemetry::ChannelMetrics metrics;
    metrics.state = telemetry::ChannelState::READY;
    metrics.buffer_depth_frames = 0;
    metrics.frame_gap_seconds = 0.0;
    metrics.decode_failure_count = 0;
    metrics_exporter_->UpdateChannelMetrics(channel_id, metrics);
  }

  // Store worker
  active_channels_[channel_id] = std::move(worker);

  response->set_success(true);
  response->set_message("Channel started with frame production");

  std::cout << "[StartChannel] Channel " << channel_id << " started successfully" << std::endl;
  return grpc::Status::OK;
}

grpc::Status PlayoutControlImpl::UpdatePlan(grpc::ServerContext* context,
                                             const UpdatePlanRequest* request,
                                             UpdatePlanResponse* response) {
  std::lock_guard<std::mutex> lock(channels_mutex_);

  const int32_t channel_id = request->channel_id();
  const std::string& plan_handle = request->plan_handle();

  std::cout << "[UpdatePlan] Request received: channel_id=" << channel_id
            << ", plan_handle=" << plan_handle << std::endl;

  // Check if channel is active
  auto it = active_channels_.find(channel_id);
  if (it == active_channels_.end()) {
    response->set_success(false);
    response->set_message("Channel not found");
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Channel is not running");
  }

  auto& worker = it->second;

  // Phase 3: Hot-swap by stopping and restarting producer and renderer
  // Future optimization: seamless transition without stopping
  std::cout << "[UpdatePlan] Stopping current producer and renderer..." << std::endl;
  
  if (worker->renderer) {
    worker->renderer->Stop();
  }
  
  if (worker->producer) {
    worker->producer->Stop();
  }

  // Clear ring buffer
  if (worker->ring_buffer) {
    worker->ring_buffer->Clear();
  }

  // Update plan handle
  worker->plan_handle = plan_handle;

  // Reconfigure and restart producer
  decode::ProducerConfig producer_config;
  producer_config.asset_uri = plan_handle;
  producer_config.target_width = 1920;
  producer_config.target_height = 1080;
  producer_config.target_fps = 30.0;
  producer_config.stub_mode = false;  // Phase 3: real decode

  worker->producer = std::make_unique<decode::FrameProducer>(
      producer_config, *worker->ring_buffer);

  if (!worker->producer->Start()) {
    response->set_success(false);
    response->set_message("Failed to restart frame producer");
    
    // Update metrics to error state
    if (metrics_exporter_) {
      telemetry::ChannelMetrics metrics;
      metrics.state = telemetry::ChannelState::ERROR_STATE;
      metrics_exporter_->UpdateChannelMetrics(channel_id, metrics);
    }
    
    return grpc::Status(grpc::StatusCode::INTERNAL, "Producer restart failed");
  }

  // Restart renderer
  if (worker->renderer) {
    if (!worker->renderer->Start()) {
      std::cerr << "[UpdatePlan] WARNING: Failed to restart renderer" << std::endl;
    }
  }

  // Update metrics
  UpdateChannelMetrics(channel_id);

  response->set_success(true);
  response->set_message("Plan updated with producer restart");

  std::cout << "[UpdatePlan] Channel " << channel_id << " plan updated successfully" << std::endl;
  return grpc::Status::OK;
}

grpc::Status PlayoutControlImpl::StopChannel(grpc::ServerContext* context,
                                              const StopChannelRequest* request,
                                              StopChannelResponse* response) {
  std::lock_guard<std::mutex> lock(channels_mutex_);

  const int32_t channel_id = request->channel_id();

  std::cout << "[StopChannel] Request received: channel_id=" << channel_id << std::endl;

  // Check if channel is active
  auto it = active_channels_.find(channel_id);
  if (it == active_channels_.end()) {
    response->set_success(false);
    response->set_message("Channel not found");
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "Channel is not running");
  }

  auto& worker = it->second;

  // Stop renderer first (consumer)
  if (worker->renderer) {
    std::cout << "[StopChannel] Stopping renderer for channel " << channel_id << std::endl;
    worker->renderer->Stop();
  }

  // Stop frame producer (producer)
  if (worker->producer) {
    std::cout << "[StopChannel] Stopping producer for channel " << channel_id << std::endl;
    worker->producer->Stop();
  }

  // Update metrics to stopped state
  if (metrics_exporter_) {
    telemetry::ChannelMetrics metrics;
    metrics.state = telemetry::ChannelState::STOPPED;
    metrics.buffer_depth_frames = 0;
    metrics.frame_gap_seconds = 0.0;
    metrics.decode_failure_count = 0;
    metrics_exporter_->UpdateChannelMetrics(channel_id, metrics);
    
    // Remove from metrics after a brief delay (handled externally)
    metrics_exporter_->RemoveChannel(channel_id);
  }

  // Remove worker (RAII cleanup handles buffer/producer destruction)
  active_channels_.erase(it);

  response->set_success(true);
  response->set_message("Channel stopped and resources released");

  std::cout << "[StopChannel] Channel " << channel_id << " stopped successfully" << std::endl;
  return grpc::Status::OK;
}

grpc::Status PlayoutControlImpl::GetVersion(grpc::ServerContext* context,
                                             const ApiVersionRequest* request,
                                             ApiVersion* response) {
  std::cout << "[GetVersion] Request received" << std::endl;

  response->set_version(kApiVersion);

  std::cout << "[GetVersion] Returning version: " << kApiVersion << std::endl;
  return grpc::Status::OK;
}

void PlayoutControlImpl::UpdateChannelMetrics(int32_t channel_id) {
  if (!metrics_exporter_) {
    return;
  }

  auto it = active_channels_.find(channel_id);
  if (it == active_channels_.end()) {
    return;
  }

  auto& worker = it->second;
  
  telemetry::ChannelMetrics metrics;
  metrics.state = telemetry::ChannelState::READY;
  
  if (worker->ring_buffer) {
    metrics.buffer_depth_frames = worker->ring_buffer->Size();
  }
  
  if (worker->producer) {
    // Update decode failure count (stub for now)
    metrics.decode_failure_count = worker->producer->GetBufferFullCount();
  }
  
  // Frame gap calculation would go here (requires MasterClock integration)
  metrics.frame_gap_seconds = 0.0;
  
  metrics_exporter_->UpdateChannelMetrics(channel_id, metrics);
}

}  // namespace playout
}  // namespace retrovue

