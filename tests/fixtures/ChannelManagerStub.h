#ifndef RETROVUE_TESTS_FIXTURES_CHANNEL_MANAGER_STUB_H_
#define RETROVUE_TESTS_FIXTURES_CHANNEL_MANAGER_STUB_H_

#include <chrono>
#include <iostream>
#include <cstdint>
#include <memory>
#include <thread>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/telemetry/MetricsExporter.h"

namespace retrovue::tests::fixtures
{

struct ChannelRuntime
{
  int32_t channel_id;
  std::unique_ptr<retrovue::buffer::FrameRingBuffer> buffer;
  std::unique_ptr<retrovue::decode::FrameProducer> producer;
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
    ChannelRuntime runtime{};
    runtime.channel_id = channel_id;
    runtime.buffer = std::make_unique<retrovue::buffer::FrameRingBuffer>(buffer_capacity);
    runtime.producer = std::make_unique<retrovue::decode::FrameProducer>(config, *runtime.buffer);
    runtime.state = retrovue::telemetry::ChannelState::BUFFERING;

    exporter.SubmitChannelMetrics(channel_id, ToMetrics(runtime));
    runtime.producer->Start();
    WaitForMinimumDepth(*runtime.buffer, exporter, runtime.channel_id, runtime.state);
    return runtime;
  }

  void RequestTeardown(ChannelRuntime& runtime,
                       retrovue::telemetry::MetricsExporter& exporter,
                       const std::string& reason,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
  {
    if (runtime.producer)
    {
      runtime.producer->RequestTeardown(timeout);
    }

    const auto start = std::chrono::steady_clock::now();
    while (runtime.producer && runtime.producer->IsRunning())
    {
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

    runtime.state = retrovue::telemetry::ChannelState::STOPPED;
    exporter.SubmitChannelMetrics(runtime.channel_id, ToMetrics(runtime));
    exporter.SubmitChannelRemoval(runtime.channel_id);
  }

  void StopChannel(ChannelRuntime& runtime,
                   retrovue::telemetry::MetricsExporter& exporter)
  {
    RequestTeardown(runtime, exporter, "ChannelManagerStub::StopChannel");
    if (runtime.buffer)
    {
      runtime.buffer->Clear();
    }
  }

private:
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

