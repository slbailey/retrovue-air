// Repository: Retrovue-playout
// Component: PlayoutControl gRPC Service Implementation
// Purpose: Implements the PlayoutControl service interface for channel lifecycle management.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SERVICE_H_
#define RETROVUE_PLAYOUT_SERVICE_H_

#include <memory>
#include <mutex>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "retrovue/playout.grpc.pb.h"
#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/renderer/FrameRenderer.h"
#include "retrovue/telemetry/MetricsExporter.h"

namespace retrovue {
namespace playout {

// ChannelWorker manages the full lifecycle of a single playout channel.
// Phase 3: Includes decoder, buffer, and renderer.
struct ChannelWorker {
  int32_t channel_id;
  std::string plan_handle;
  int32_t port;
  
  std::unique_ptr<buffer::FrameRingBuffer> ring_buffer;
  std::unique_ptr<decode::FrameProducer> producer;
  std::unique_ptr<renderer::FrameRenderer> renderer;
  
  ChannelWorker(int32_t id, const std::string& plan, int32_t p)
      : channel_id(id), plan_handle(plan), port(p) {}
};

// PlayoutControlImpl implements the gRPC service defined in playout.proto.
// Phase 3: Full decode -> render -> metrics pipeline.
class PlayoutControlImpl final : public PlayoutControl::Service {
 public:
  // Constructs the service with a shared metrics exporter.
  explicit PlayoutControlImpl(std::shared_ptr<telemetry::MetricsExporter> metrics_exporter);
  ~PlayoutControlImpl() override;

  // Disable copy and move
  PlayoutControlImpl(const PlayoutControlImpl&) = delete;
  PlayoutControlImpl& operator=(const PlayoutControlImpl&) = delete;

  // RPC implementations
  grpc::Status StartChannel(grpc::ServerContext* context,
                            const StartChannelRequest* request,
                            StartChannelResponse* response) override;

  grpc::Status UpdatePlan(grpc::ServerContext* context,
                          const UpdatePlanRequest* request,
                          UpdatePlanResponse* response) override;

  grpc::Status StopChannel(grpc::ServerContext* context,
                           const StopChannelRequest* request,
                           StopChannelResponse* response) override;

  grpc::Status GetVersion(grpc::ServerContext* context,
                          const ApiVersionRequest* request,
                          ApiVersion* response) override;

 private:
  // Updates metrics for a channel based on current state.
  void UpdateChannelMetrics(int32_t channel_id);

  // Metrics exporter (shared across all channels)
  std::shared_ptr<telemetry::MetricsExporter> metrics_exporter_;
  
  // Active channel workers
  std::mutex channels_mutex_;
  std::unordered_map<int32_t, std::unique_ptr<ChannelWorker>> active_channels_;
};

}  // namespace playout
}  // namespace retrovue

#endif  // RETROVUE_PLAYOUT_SERVICE_H_

