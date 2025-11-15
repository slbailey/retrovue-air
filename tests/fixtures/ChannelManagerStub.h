#ifndef RETROVUE_TESTS_FIXTURES_CHANNEL_MANAGER_STUB_H_
#define RETROVUE_TESTS_FIXTURES_CHANNEL_MANAGER_STUB_H_

#include <chrono>
#include <iostream>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/telemetry/MetricsExporter.h"

namespace retrovue::tests::fixtures
{

// Internal storage for channel runtime - uses shared_ptr for buffer and producer
// so they can be shared with the returned ChannelRuntime
struct ChannelRuntimeStorage {
  int32_t channel_id;
  std::shared_ptr<retrovue::buffer::FrameRingBuffer> buffer;
  std::shared_ptr<retrovue::decode::FrameProducer> producer;
  retrovue::telemetry::ChannelState state;
};

struct ChannelRuntime
{
  int32_t channel_id;
  std::shared_ptr<retrovue::buffer::FrameRingBuffer> buffer;
  std::shared_ptr<retrovue::decode::FrameProducer> producer;
  retrovue::telemetry::ChannelState state;
};

class ChannelManagerStub
{
public:
  ChannelRuntime StartChannel(int32_t channel_id,
                              const retrovue::decode::ProducerConfig& config,
                              retrovue::telemetry::MetricsExporter& exporter,
                              std::size_t buffer_capacity = 30)
  {
    // BC-003: Idempotent - if channel already exists, return existing runtime
    auto it = active_channels_.find(channel_id);
    if (it != active_channels_.end()) {
      // Channel already started - return a runtime with shared pointers
      return CreateRuntimeFromStorage(*(it->second));
    }

    // Create new channel runtime storage
    auto storage = std::make_shared<ChannelRuntimeStorage>();
    storage->channel_id = channel_id;
    storage->buffer = std::make_shared<retrovue::buffer::FrameRingBuffer>(buffer_capacity);
    storage->producer = std::make_shared<retrovue::decode::FrameProducer>(config, *storage->buffer);
    storage->state = retrovue::telemetry::ChannelState::BUFFERING;

    exporter.SubmitChannelMetrics(channel_id, ToMetricsFromStorage(*storage));
    storage->producer->Start();
    WaitForMinimumDepth(*storage->buffer, exporter, storage->channel_id, storage->state);
    
    // Store in active channels map for idempotency
    active_channels_[channel_id] = storage;
    
    // Return a runtime with shared ownership of buffer and producer
    return CreateRuntimeFromStorage(*storage);
  }

  void RequestTeardown(ChannelRuntime& runtime,
                       retrovue::telemetry::MetricsExporter& exporter,
                       const std::string& reason,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500),
                       bool remove_metrics = true)
  {
    if (runtime.producer)
    {
      runtime.producer->RequestTeardown(timeout);
    }

    // Wait for producer to stop and drain buffer
    const auto start = std::chrono::steady_clock::now();
    while (runtime.producer && runtime.producer->IsRunning())
    {
      // During teardown, drain the buffer periodically to help it empty
      if (runtime.buffer && !runtime.buffer->IsEmpty())
      {
        retrovue::buffer::Frame frame;
        // Pop a few frames to help drainage
        for (int i = 0; i < 5 && runtime.buffer->Pop(frame); ++i)
        {
          // Drain frames
        }
      }
      
      if (std::chrono::steady_clock::now() - start > timeout)
      {
        std::cerr << "[ChannelManagerStub] Teardown timed out for channel "
                  << runtime.channel_id << ", forcing stop" << std::endl;
        runtime.producer->ForceStop();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (runtime.producer)
    {
      runtime.producer->Stop();
    }

    // Final drain of any remaining frames in buffer
    if (runtime.buffer)
    {
      retrovue::buffer::Frame frame;
      while (runtime.buffer->Pop(frame))
      {
        // Drain all remaining frames
      }
    }

    runtime.state = retrovue::telemetry::ChannelState::STOPPED;
    exporter.SubmitChannelMetrics(runtime.channel_id, ToMetrics(runtime));
    
    // Remove metrics if requested (BC-007 expects metrics to be removed after teardown)
    if (remove_metrics) {
      exporter.SubmitChannelRemoval(runtime.channel_id);
    }
    // Note: If remove_metrics=false, metrics remain available with STOPPED state
    // for tests that check state after stop (BC-003, BC-005)
  }

  // Helper to create ChannelRuntime from ChannelRuntimeStorage
  // Simply shares the shared_ptrs from storage
  static ChannelRuntime CreateRuntimeFromStorage(const ChannelRuntimeStorage& storage) {
    ChannelRuntime runtime;
    runtime.channel_id = storage.channel_id;
    runtime.state = storage.state;
    runtime.buffer = storage.buffer;
    runtime.producer = storage.producer;
    return runtime;
  }

  void StopChannel(ChannelRuntime& runtime,
                   retrovue::telemetry::MetricsExporter& exporter)
  {
    // BC-003: Idempotent - if channel is already stopped, this is a no-op
    if (runtime.state == retrovue::telemetry::ChannelState::STOPPED) {
      return;  // Already stopped, safe to call again
    }

    // Look up the actual storage in the map
    auto it = active_channels_.find(runtime.channel_id);
    if (it != active_channels_.end()) {
      // Use the actual storage from the map
      auto& storage = *(it->second);
      
      // Convert storage to runtime for RequestTeardown
      ChannelRuntime actual_runtime = CreateRuntimeFromStorage(storage);
      
      // StopChannel gracefully stops the channel and removes metrics to avoid stale state (MT-005)
      RequestTeardown(actual_runtime, exporter, "ChannelManagerStub::StopChannel", 
                      std::chrono::milliseconds(500), /*remove_metrics=*/true);
      if (storage.buffer)
      {
        storage.buffer->Clear();
      }
      
      // Update storage state to STOPPED (RequestTeardown sets actual_runtime.state but not storage.state)
      storage.state = retrovue::telemetry::ChannelState::STOPPED;
      
      // Update the parameter runtime state and buffer for the caller (BC-005)
      runtime.state = storage.state;
      // BC-005: Test expects runtime.buffer to be non-null after StopChannel
      // Share the buffer and producer from storage
      runtime.buffer = storage.buffer;
      runtime.producer = storage.producer;
      
      // Remove from active channels map
      active_channels_.erase(runtime.channel_id);
    } else {
      // Channel not found in map - already stopped or never started
      runtime.state = retrovue::telemetry::ChannelState::STOPPED;
    }
  }

private:
  // Track active channels for idempotency (BC-003)
  // Note: We store ChannelRuntimeStorage with shared_ptr for buffer/producer
  // so they can be shared with returned ChannelRuntime objects
  std::unordered_map<int32_t, std::shared_ptr<ChannelRuntimeStorage>> active_channels_;
  
  static void WaitForMinimumDepth(retrovue::buffer::FrameRingBuffer& buffer,
                                  retrovue::telemetry::MetricsExporter& exporter,
                                  int32_t channel_id,
                                  retrovue::telemetry::ChannelState& state)
  {
    constexpr std::size_t kReadyDepth = 3;
    constexpr auto kTimeout = std::chrono::seconds(2);
    const auto start = std::chrono::steady_clock::now();

    while (buffer.Size() < kReadyDepth)
    {
      if (std::chrono::steady_clock::now() - start > kTimeout)
      {
        state = retrovue::telemetry::ChannelState::BUFFERING;
        exporter.SubmitChannelMetrics(channel_id,
                                      ToMetrics(buffer.Size(),
                                                state));
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    state = retrovue::telemetry::ChannelState::READY;
    exporter.SubmitChannelMetrics(channel_id,
                                  ToMetrics(buffer.Size(),
                                            state));
  }

  static retrovue::telemetry::ChannelMetrics ToMetrics(const ChannelRuntime& runtime)
  {
    return ToMetrics(runtime.buffer ? runtime.buffer->Size() : 0,
                     runtime.state);
  }

  static retrovue::telemetry::ChannelMetrics ToMetricsFromStorage(const ChannelRuntimeStorage& storage)
  {
    return ToMetrics(storage.buffer ? storage.buffer->Size() : 0,
                     storage.state);
  }

  static retrovue::telemetry::ChannelMetrics ToMetrics(std::size_t buffer_depth,
                                                       retrovue::telemetry::ChannelState state)
  {
    retrovue::telemetry::ChannelMetrics metrics{};
    metrics.state = state;
    metrics.buffer_depth_frames = state == retrovue::telemetry::ChannelState::STOPPED
                                      ? 0
                                      : static_cast<uint64_t>(buffer_depth);
    metrics.frame_gap_seconds = 0.0;
    metrics.decode_failure_count = 0;
    return metrics;
  }
};

} // namespace retrovue::tests::fixtures

#endif // RETROVUE_TESTS_FIXTURES_CHANNEL_MANAGER_STUB_H_
