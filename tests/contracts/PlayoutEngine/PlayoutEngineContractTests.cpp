#include "../../BaseContractTest.h"
#include "../ContractRegistryEnvironment.h"

#include <chrono>
#include <thread>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/renderer/FrameRenderer.h"
#include "retrovue/telemetry/MetricsExporter.h"
#include "../../fixtures/ChannelManagerStub.h"
#include "timing/TestMasterClock.h"

using namespace retrovue;
using namespace retrovue::tests;
using namespace retrovue::tests::fixtures;

namespace
{

using retrovue::tests::RegisterExpectedDomainCoverage;

const bool kRegisterCoverage = []() {
  RegisterExpectedDomainCoverage(
      "PlayoutEngine",
      {"BC-001", "BC-002", "BC-003", "BC-004", "BC-005", "BC-006"});
  return true;
}();

class PlayoutEngineContractTest : public BaseContractTest
{
protected:
  [[nodiscard]] std::string DomainName() const override
  {
    return "PlayoutEngine";
  }

  [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override
  {
    return {
        "BC-001",
        "BC-002",
        "BC-003",
        "BC-004",
        "BC-005",
        "BC-006"};
  }
};

// Rule: BC-001 Frame timing accuracy (PlayoutEngineDomain.md §BC-001)
TEST_F(PlayoutEngineContractTest, BC_001_FrameTimingAlignsWithMasterClock)
{
  buffer::FrameRingBuffer buffer(/*capacity=*/120);
  const int64_t pts_step = 33'366;
  for (int i = 0; i < 120; ++i)
  {
    buffer::Frame frame;
    frame.metadata.pts = i * pts_step;
    frame.metadata.duration = 1.0 / 29.97;
    ASSERT_TRUE(buffer.Push(frame));
  }

  auto metrics = std::make_shared<telemetry::MetricsExporter>(0);
  auto clock = std::make_shared<retrovue::timing::TestMasterClock>();
  const int64_t epoch = 1'700'001'000'000'000;
  clock->SetEpochUtcUs(epoch);
  clock->SetRatePpm(0.0);
  clock->SetNow(epoch + 2'000, 0.0); // 2 ms skew ahead

  constexpr int32_t kChannelId = 2401;
  telemetry::ChannelMetrics seed{};
  seed.state = telemetry::ChannelState::READY;
  metrics->SubmitChannelMetrics(kChannelId, seed);

  renderer::RenderConfig config;
  config.mode = renderer::RenderMode::HEADLESS;
  auto renderer = renderer::FrameRenderer::Create(
      config, buffer, clock, metrics, kChannelId);
  ASSERT_NE(renderer, nullptr);
  ASSERT_TRUE(renderer->Start());

  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  telemetry::ChannelMetrics snapshot{};
  ASSERT_TRUE(metrics->GetChannelMetrics(kChannelId, snapshot));
  EXPECT_LT(std::abs(snapshot.frame_gap_seconds), 0.0167)
      << "Frame gap must stay within one frame period";

  clock->AdvanceSeconds(0.05);
  renderer->Stop();

  const auto &stats = renderer->GetStats();
  EXPECT_GE(stats.frames_rendered, 1u);
}

// Rule: BC-005 Resource Cleanup (PlayoutEngineDomain.md §BC-005)
TEST_F(PlayoutEngineContractTest, BC_005_ChannelStopReleasesResources)
{
  telemetry::MetricsExporter exporter(/*port=*/0);
  ChannelManagerStub manager;

  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/channel";
  config.target_fps = 29.97;

  auto runtime = manager.StartChannel(201, config, exporter, /*buffer_capacity=*/12);

  // Verify channel is running before stop
  telemetry::ChannelMetrics metrics_before{};
  ASSERT_TRUE(exporter.GetChannelMetrics(201, metrics_before));
  EXPECT_NE(metrics_before.state, telemetry::ChannelState::STOPPED);

  manager.StopChannel(runtime, exporter);

  // After stop, metrics are removed to avoid stale state (MT-005)
  telemetry::ChannelMetrics metrics_after{};
  EXPECT_FALSE(exporter.GetChannelMetrics(201, metrics_after))
      << "Metrics should be removed after channel stop";
  
  // Verify resources are released
  ASSERT_NE(runtime.buffer, nullptr);
  EXPECT_TRUE(runtime.buffer->IsEmpty());
}

// Rule: BC-003 Control operations are idempotent (PlayoutEngineDomain.md §BC-003)
TEST_F(PlayoutEngineContractTest, BC_003_ControlOperationsAreIdempotent)
{
  telemetry::MetricsExporter exporter(/*port=*/0);
  ChannelManagerStub manager;

  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/idempotent";
  config.target_fps = 29.97;

  auto runtime_first = manager.StartChannel(210, config, exporter, /*buffer_capacity=*/8);

  telemetry::ChannelMetrics metrics{};
  ASSERT_TRUE(exporter.GetChannelMetrics(210, metrics));
  EXPECT_EQ(metrics.state, telemetry::ChannelState::READY);

  auto runtime_second = manager.StartChannel(210, config, exporter, /*buffer_capacity=*/8);
  ASSERT_TRUE(exporter.GetChannelMetrics(210, metrics));
  EXPECT_EQ(metrics.state, telemetry::ChannelState::READY)
      << "Repeated StartChannel must be a no-op";

  manager.StopChannel(runtime_first, exporter);
  // After first stop, metrics are removed (MT-005)
  EXPECT_FALSE(exporter.GetChannelMetrics(210, metrics))
      << "Metrics should be removed after channel stop";
  
  manager.StopChannel(runtime_first, exporter); // idempotent stop - should be safe to call again
  // Metrics should still be removed after idempotent stop
  EXPECT_FALSE(exporter.GetChannelMetrics(210, metrics))
      << "Metrics should remain removed after idempotent stop";

  manager.StopChannel(runtime_second, exporter);
}

// Rule: BC-004 Graceful degradation isolates channel errors (PlayoutEngineDomain.md §BC-004)
TEST_F(PlayoutEngineContractTest, BC_004_ChannelErrorIsolation)
{
  telemetry::MetricsExporter exporter(/*port=*/0);
  ChannelManagerStub manager;

  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/error_isolation";
  config.target_fps = 30.0;

  auto channel_a = manager.StartChannel(220, config, exporter, /*buffer_capacity=*/8);
  auto channel_b = manager.StartChannel(221, config, exporter, /*buffer_capacity=*/8);

  telemetry::ChannelMetrics metrics_a{};
  telemetry::ChannelMetrics metrics_b{};
  ASSERT_TRUE(exporter.GetChannelMetrics(220, metrics_a));
  ASSERT_TRUE(exporter.GetChannelMetrics(221, metrics_b));
  EXPECT_EQ(metrics_a.state, telemetry::ChannelState::READY);
  EXPECT_EQ(metrics_b.state, telemetry::ChannelState::READY);

  telemetry::ChannelMetrics error_state{};
  error_state.state = telemetry::ChannelState::ERROR_STATE;
  error_state.decode_failure_count = 1;
  exporter.SubmitChannelMetrics(221, error_state);

  ASSERT_TRUE(exporter.GetChannelMetrics(221, metrics_b));
  EXPECT_EQ(metrics_b.state, telemetry::ChannelState::ERROR_STATE);

  ASSERT_TRUE(exporter.GetChannelMetrics(220, metrics_a));
  EXPECT_EQ(metrics_a.state, telemetry::ChannelState::READY)
      << "Error on one channel must not impact other channels";

  manager.StopChannel(channel_a, exporter);
  manager.StopChannel(channel_b, exporter);
}

// Rule: BC-002 Buffer Depth Guarantees (PlayoutEngineDomain.md §BC-002)
TEST_F(PlayoutEngineContractTest, BC_002_BufferDepthRemainsWithinCapacity)
{
  telemetry::MetricsExporter exporter(/*port=*/0);
  ChannelManagerStub manager;

  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/buffer";
  config.target_fps = 30.0;

  constexpr std::size_t kCapacity = 10;
  auto runtime = manager.StartChannel(202, config, exporter, kCapacity);

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const auto depth = runtime.buffer->Size();
  EXPECT_LE(depth, kCapacity);
  EXPECT_GE(depth, 1u);

  manager.StopChannel(runtime, exporter);
}

// Rule: BC-007 Graceful Teardown Handshake (Phase 5d)
TEST_F(PlayoutEngineContractTest, BC_007_TeardownHandshakeDrainsProducer)
{
  telemetry::MetricsExporter exporter(/*port=*/0);
  ChannelManagerStub manager;

  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/teardown";
  config.target_fps = 30.0;

  auto runtime = manager.StartChannel(230, config, exporter, /*buffer_capacity=*/8);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));

  manager.RequestTeardown(runtime, exporter, "viewer_count_zero");

  ASSERT_TRUE(runtime.buffer);
  EXPECT_TRUE(runtime.buffer->IsEmpty());

  telemetry::ChannelMetrics metrics{};
  EXPECT_FALSE(exporter.GetChannelMetrics(230, metrics))
      << "Teardown should remove channel metrics to avoid stale active state";
}

// Rule: BC-006 Monotonic PTS (PlayoutEngineDomain.md §BC-006)
TEST_F(PlayoutEngineContractTest, BC_006_FramePtsRemainMonotonic)
{
  buffer::FrameRingBuffer buffer(/*capacity=*/8);
  decode::ProducerConfig config;
  config.stub_mode = true;
  config.asset_uri = "contract://playout/pts";
  config.target_fps = 30.0;

  decode::FrameProducer producer(config, buffer);
  ASSERT_TRUE(producer.Start());

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  producer.Stop();

  buffer::Frame previous_frame;
  bool has_previous = false;
  buffer::Frame frame;
  while (buffer.Pop(frame))
  {
    if (has_previous)
    {
      EXPECT_GT(frame.metadata.pts, previous_frame.metadata.pts);
    }
    has_previous = true;
    previous_frame = frame;
  }
  EXPECT_TRUE(has_previous);
}

} // namespace

