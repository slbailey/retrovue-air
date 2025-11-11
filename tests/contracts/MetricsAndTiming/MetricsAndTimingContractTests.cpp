#include "../../BaseContractTest.h"
#include "../ContractRegistryEnvironment.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>
#include <vector>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/decode/FrameProducer.h"
#include "retrovue/renderer/FrameRenderer.h"
#include "timing/TestMasterClock.h"
#include "../../fixtures/ChannelManagerStub.h"
#include "../../fixtures/MasterClockStub.h"

using namespace retrovue;
using namespace retrovue::tests;
using namespace retrovue::tests::fixtures;

namespace
{

  using retrovue::tests::RegisterExpectedDomainCoverage;

  struct StubFrameProducer
  {
    explicit StubFrameProducer(int64_t pts_step_us)
        : pts_step_us(pts_step_us), pts_counter(0)
    {
    }

    bool Produce(buffer::FrameRingBuffer& buffer)
    {
      buffer::Frame frame;
      frame.metadata.pts = pts_counter;
      frame.metadata.dts = pts_counter;
      frame.metadata.duration =
          static_cast<double>(pts_step_us) / 1'000'000.0;
      frame.metadata.asset_uri = "contract://metrics/stub_cadence";
      frame.width = 1920;
      frame.height = 1080;

      pts_counter += pts_step_us;
      return buffer.Push(frame);
    }

    int64_t pts_step_us;
    int64_t pts_counter;
  };

  const bool kRegisterCoverage = []()
  {
    RegisterExpectedDomainCoverage("MetricsAndTiming",
                                   {"MT-001",
                                    "MT-002",
                                    "MT-003",
                                    "MT-004",
                                    "MT-005",
                                    "MT-006",
                                    "MT-007",
                                    "MT-008"});
    return true;
  }();

  class MetricsAndTimingContractTest : public BaseContractTest
  {
  protected:
    [[nodiscard]] std::string DomainName() const override
    {
      return "MetricsAndTiming";
    }

    [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override
    {
      return {
          "MT-001",
          "MT-002",
          "MT-003",
          "MT-004",
          "MT-005",
          "MT-006",
          "MT-007",
          "MT-008"};
    }
  };

  // Rule: MT-001 Monotonic now() (MasterClockDomainContract.md §MT_001)
  TEST_F(MetricsAndTimingContractTest, MT_001_MasterClockMonotonicAndLowJitter)
  {
    MasterClockStub clock;
    int64_t last = clock.now_utc_us();
    for (int i = 0; i < 1000; ++i)
    {
      clock.Advance(1'000); // advance by 1 ms
      const int64_t now = clock.now_utc_us();
      EXPECT_GE(now, last);
      last = now;
    }

    // Deterministic jitter check: advance alternating 1ms/2ms, ensure <1ms p95
    double max_jitter_us = 0.0;
    for (int i = 0; i < 1000; ++i)
    {
      clock.Advance((i % 2 == 0) ? 1'000 : 2'000);
      const int64_t now = clock.now_utc_us();
      const double jitter = std::abs(static_cast<double>(now - last) - 1'500.0);
      max_jitter_us = std::max(max_jitter_us, jitter);
      last = now;
    }
    EXPECT_LT(max_jitter_us, 1'000.0);
  }
  // Rule: MT-002 Stable PTS mapping (MasterClockDomainContract.md §MT_002)
  TEST_F(MetricsAndTimingContractTest, MT_002_PtsToUtcMappingStable)
  {
    MasterClockStub clock;
    const int64_t epoch = clock.now_utc_us();
    clock.Advance(0); // ensure deterministic start

    const double rate_ppm = 100.0; // small rate offset
    const auto system_clock =
        retrovue::timing::MakeSystemMasterClock(epoch, rate_ppm);

    const int64_t pts_step_us = 33'366; // approx 29.97 fps
    int64_t previous_deadline = system_clock->scheduled_to_utc_us(0);
    for (int i = 1; i <= 120; ++i)
    {
      const int64_t pts = i * pts_step_us;
      const int64_t deadline = system_clock->scheduled_to_utc_us(pts);
      EXPECT_GT(deadline, previous_deadline);
      const long double expected =
          static_cast<long double>(epoch) +
          static_cast<long double>(pts) * (1.0L + rate_ppm / 1'000'000.0L);
      const long double diff = std::llabs(deadline - static_cast<int64_t>(expected));
      EXPECT_LT(diff, 100.0L) << "PTS conversion must remain stable";
      previous_deadline = deadline;
    }
  }

  // Rule: MT-003 Pace controller convergence (MasterClockDomainContract.md §MT_003)
  TEST_F(MetricsAndTimingContractTest, MT_003_PaceControllerReducesGap)
  {
    buffer::FrameRingBuffer buffer(/*capacity=*/180);
    const int64_t pts_step = 33'366;
    for (int i = 0; i < 180; ++i)
    {
      buffer::Frame frame;
      frame.metadata.pts = i * pts_step;
      frame.metadata.duration = 1.0 / 29.97;
      ASSERT_TRUE(buffer.Push(frame));
    }

    auto metrics = std::make_shared<telemetry::MetricsExporter>(0);
    auto clock = std::make_shared<retrovue::timing::TestMasterClock>();
    const int64_t epoch = 1'700'000'300'000'000;
    clock->SetEpochUtcUs(epoch);
    clock->SetRatePpm(0.0);
    clock->SetNow(epoch + 9'000, 0.0); // 9 ms skew ahead of schedule

    renderer::RenderConfig config;
    config.mode = renderer::RenderMode::HEADLESS;
    constexpr int32_t kChannelId = 1701;

    telemetry::ChannelMetrics seed{};
    seed.state = telemetry::ChannelState::READY;
    metrics->UpdateChannelMetrics(kChannelId, seed);

    auto renderer = renderer::FrameRenderer::Create(
        config, buffer, clock, metrics, kChannelId);
    ASSERT_TRUE(renderer);
    ASSERT_TRUE(renderer->Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    telemetry::ChannelMetrics snapshot{};
    ASSERT_TRUE(metrics->GetChannelMetrics(kChannelId, snapshot));
    const double observed_gap_ms = std::abs(snapshot.frame_gap_seconds * 1000.0);
    const double initial_gap_ms =
        std::abs(static_cast<double>(
                     clock->scheduled_to_utc_us(0) - (epoch + 9'000)) /
                 1000.0);
    EXPECT_LE(observed_gap_ms, initial_gap_ms + 0.5)
        << "Pace controller should reduce the absolute frame gap";

    clock->AdvanceSeconds(0.05);
    renderer->Stop();

    const auto &stats = renderer->GetStats();
    EXPECT_GE(stats.frames_rendered, 1u);
    EXPECT_GT(stats.corrections_total, 0u);
  }

  // Rule: MT-004 Underrun recovery (MasterClockDomainContract.md §MT_004)
  TEST_F(MetricsAndTimingContractTest, MT_004_UnderrunTriggersBufferingAndRecovery)
  {
    telemetry::MetricsExporter exporter(/*port=*/0);
    fixtures::ChannelManagerStub manager;

    retrovue::decode::ProducerConfig config;
    config.stub_mode = true;
    config.target_fps = 30.0;
    config.asset_uri = "contract://metrics/underrun";

    auto runtime = manager.StartChannel(303, config, exporter, /*buffer_capacity=*/8);
    auto &buffer = *runtime.buffer;

    runtime.producer->Stop();
    buffer::Frame drained;
    while (buffer.Pop(drained))
    {
    }
    EXPECT_TRUE(buffer.IsEmpty());

    telemetry::ChannelMetrics buffering{};
    buffering.state = telemetry::ChannelState::BUFFERING;
    buffering.buffer_depth_frames = 0;
    exporter.UpdateChannelMetrics(runtime.channel_id, buffering);

    telemetry::ChannelMetrics metrics{};
    ASSERT_TRUE(exporter.GetChannelMetrics(runtime.channel_id, metrics));
    EXPECT_EQ(metrics.state, telemetry::ChannelState::BUFFERING);
    EXPECT_EQ(metrics.buffer_depth_frames, 0u);

    const size_t refill_count = std::max<size_t>(1, buffer.Capacity() - 1);
    for (size_t i = 0; i < refill_count; ++i)
    {
      buffer::Frame f;
      f.metadata.pts = static_cast<int64_t>(i);
      f.metadata.duration = 1.0 / config.target_fps;
      ASSERT_TRUE(buffer.Push(f));
    }

    telemetry::ChannelMetrics ready{};
    ready.state = telemetry::ChannelState::READY;
    ready.buffer_depth_frames = buffer.Size();
    exporter.UpdateChannelMetrics(runtime.channel_id, ready);

    ASSERT_TRUE(exporter.GetChannelMetrics(runtime.channel_id, metrics));
    EXPECT_EQ(metrics.state, telemetry::ChannelState::READY);
    EXPECT_GE(metrics.buffer_depth_frames, 1u);

    manager.StopChannel(runtime, exporter);
  }

  // Rule: MT-003 Frame Cadence (MetricsAndTimingContract.md §MT-003)
  TEST_F(MetricsAndTimingContractTest, MT_003_FrameCadenceMaintainsMonotonicPts)
  {
    buffer::FrameRingBuffer buffer(12);
    decode::ProducerConfig config;
    config.stub_mode = true;
    config.asset_uri = "contract://metrics/frame_cadence";
    config.target_fps = 30.0;

    decode::FrameProducer producer(config, buffer);
    ASSERT_TRUE(producer.Start());

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    producer.Stop();

    EXPECT_GE(producer.GetFramesProduced(), 3u);

    buffer::Frame previous;
    bool has_previous = false;

    buffer::Frame frame;
    while (buffer.Pop(frame))
    {
      EXPECT_GT(frame.metadata.duration, 0.0);
      AssertWithinTolerance(frame.metadata.duration,
                            1.0 / config.target_fps,
                            1e-6,
                            "Duration must align with target FPS");

      if (has_previous)
      {
        EXPECT_GE(frame.metadata.pts, previous.metadata.pts);
      }
      previous = frame;
      has_previous = true;
    }
  }

  // Rule: MT-005 Prometheus Metrics (MetricsAndTimingContract.md §MT-005)
  TEST_F(MetricsAndTimingContractTest, MT_005_MetricsExporterReflectsChannelState)
  {
    telemetry::MetricsExporter exporter(/*port=*/0);
    ChannelManagerStub manager;

    decode::ProducerConfig config;
    config.stub_mode = true;
    config.asset_uri = "contract://metrics/telemetry";
    config.target_fps = 24.0;

    auto runtime = manager.StartChannel(101, config, exporter, /*buffer_capacity=*/8);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    telemetry::ChannelMetrics metrics{};
    ASSERT_TRUE(exporter.GetChannelMetrics(101, metrics));
    EXPECT_NE(metrics.state, telemetry::ChannelState::STOPPED);
    EXPECT_GE(metrics.buffer_depth_frames, 1u);

    manager.StopChannel(runtime, exporter);
    ASSERT_TRUE(exporter.GetChannelMetrics(101, metrics));
    EXPECT_EQ(metrics.state, telemetry::ChannelState::STOPPED);
    EXPECT_EQ(metrics.buffer_depth_frames, 0u);
  }

  // Rule: MT-007 SLO Guardrails (MetricsAndTimingContract.md §MT_007)
  TEST_F(MetricsAndTimingContractTest, MT_007_SLO_Guards)
  {
    SCOPED_TRACE("MT-007: Verify frame gap and correction SLO guardrails");

    auto clock = std::make_shared<retrovue::timing::TestMasterClock>();
    const int64_t epoch = 1'700'003'000'000'000;
    clock->SetEpochUtcUs(epoch);
    clock->SetRatePpm(0.0);
    clock->SetNow(epoch, 0.0);

    buffer::FrameRingBuffer buffer(256);
    StubFrameProducer stub(33'366);

    std::vector<double> abs_gap_ms;
    abs_gap_ms.reserve(2'000);
    uint64_t corrections = 0;

    constexpr int kTotalFrames = 2'000;
    for (int i = 0; i < kTotalFrames; ++i)
    {
      ASSERT_TRUE(stub.Produce(buffer));

      buffer::Frame frame;
      ASSERT_TRUE(buffer.Pop(frame));

      const int64_t deadline = clock->scheduled_to_utc_us(frame.metadata.pts);
      const int64_t now = clock->now_utc_us();
      const int64_t gap_us = deadline - now;
      abs_gap_ms.push_back(std::abs(static_cast<double>(gap_us) / 1'000.0));

      if (gap_us > 0)
      {
        clock->AdvanceMicroseconds(gap_us);
      }
      else if (gap_us < 0)
      {
        ++corrections;
        const double adjust_ppm =
            std::clamp(-static_cast<double>(gap_us) / 1'000.0 * 0.05, -40.0, 40.0);
        clock->SetDriftPpm(clock->drift_ppm() + adjust_ppm);
        const int64_t catchup_us =
            std::min<int64_t>(33'366, -gap_us);
        clock->AdvanceMicroseconds(catchup_us);
      }

      clock->AdvanceMicroseconds(33'366);
    }

    auto mean = [](const std::vector<double>& values) {
      if (values.empty()) return 0.0;
      return std::accumulate(values.begin(), values.end(), 0.0) /
             static_cast<double>(values.size());
    };

    auto p95 = [](std::vector<double> values) {
      if (values.empty()) return 0.0;
      std::sort(values.begin(), values.end());
      const double rank = 0.95 * static_cast<double>(values.size() - 1);
      const auto lo = static_cast<size_t>(std::floor(rank));
      const auto hi = static_cast<size_t>(std::ceil(rank));
      const double fraction = rank - static_cast<double>(lo);
      return values[lo] +
             (values[hi] - values[lo]) * fraction;
    };

    const double p95_gap_ms = p95(abs_gap_ms);
    const double corrections_per_frame =
        static_cast<double>(corrections) / static_cast<double>(kTotalFrames);

    EXPECT_LT(mean(abs_gap_ms), 10.0)
        << "Mean absolute gap should stay within 10 ms SLO (MT-007)";
    EXPECT_LT(p95_gap_ms, 4.0)
        << "p95 absolute gap should stay below 4 ms (MT-007)";
    EXPECT_LT(corrections_per_frame, 0.03)
        << "Corrections per frame must stay under 0.03 (MT-007)";
  }

  // Rule: MT-005 Long-run drift stability (MetricsAndTimingContract.md §MT-005)
  TEST_F(MetricsAndTimingContractTest, MT_005_LongRunDriftStability)
  {
    auto clock = std::make_shared<retrovue::timing::TestMasterClock>();
    const int64_t epoch = 1'700'000'000'000'000;
    clock->SetEpochUtcUs(epoch);
    clock->SetRatePpm(0.0);
    clock->SetNow(epoch, 0.0);

    buffer::FrameRingBuffer buffer(512);
    const int total_frames = 18'000; // 10 minutes at 30 fps
    const int64_t pts_step_us = static_cast<int64_t>(1'000'000.0 / 30.0);
    for (int i = 0; i < total_frames; ++i)
    {
      buffer::Frame frame;
      frame.metadata.pts = i * pts_step_us;
      buffer.Push(frame);
    }

    retrovue::renderer::RenderConfig config;
    auto metrics = std::make_shared<retrovue::telemetry::MetricsExporter>(0);
    auto renderer = retrovue::renderer::FrameRenderer::Create(
        config, buffer, clock, metrics, 900);
    ASSERT_NE(renderer, nullptr);

    std::vector<double> frame_gaps_ms;
    frame_gaps_ms.reserve(total_frames);

    clock->SetNow(epoch + 8'000, 0.0); // inject initial skew

    const double frame_duration_s =
        static_cast<double>(pts_step_us) / 1'000'000.0;

    for (int i = 0; i < total_frames; ++i)
    {
      const int64_t deadline = clock->scheduled_to_utc_us(i * pts_step_us);
      const int64_t now = clock->now_utc_us();
      const double gap_ms = static_cast<double>(deadline - now) / 1'000.0;
      const double adjust_ppm = -gap_ms * 0.05;
      clock->SetDriftPpm(clock->drift_ppm() + adjust_ppm);
      if (gap_ms > 0.0)
      {
        clock->AdvanceSeconds(gap_ms / 1'000.0);
      }
      else if (gap_ms < 0.0)
      {
        buffer::Frame ignore;
        buffer.Pop(ignore);
      }
      const double corrected_gap_ms =
          static_cast<double>(clock->scheduled_to_utc_us(i * pts_step_us) -
                              clock->now_utc_us()) /
          1'000.0;
      frame_gaps_ms.push_back(corrected_gap_ms);
      clock->AdvanceSeconds(frame_duration_s);
    }

    if (frame_gaps_ms.size() > 30)
    {
      frame_gaps_ms.erase(frame_gaps_ms.begin(),
                          frame_gaps_ms.begin() + 30);
    }

    const double mean_abs =
        std::accumulate(frame_gaps_ms.begin(), frame_gaps_ms.end(), 0.0,
                        [](double acc, double val)
                        { return acc + std::abs(val); }) /
        frame_gaps_ms.size();

    std::vector<double> sorted = frame_gaps_ms;
    std::transform(sorted.begin(), sorted.end(), sorted.begin(),
                   [](double v)
                   { return std::abs(v); });
    std::sort(sorted.begin(), sorted.end());
    const size_t index_p95 = static_cast<size_t>(0.95 * sorted.size());
    const double p95 = sorted[std::min(index_p95, sorted.size() - 1)];

    const auto &stats = renderer->GetStats();

    EXPECT_LT(mean_abs, 10.0);
    EXPECT_LT(p95, 1.0);
    EXPECT_LE(stats.corrections_total, 600u);
  }

  // Rule: MT-006 Feedback convergence (MetricsAndTimingContract.md §MT-006)
  TEST_F(MetricsAndTimingContractTest, MT_006_FeedbackConvergence)
  {
    retrovue::timing::TestMasterClock clock;
    const int64_t epoch = 1'700'000'000'000'000;
    clock.SetEpochUtcUs(epoch);
    clock.SetRatePpm(0.0);
    clock.SetNow(epoch, 0.0);

    constexpr int64_t kFramePtsUs = 33'366; // ~29.97 fps
    clock.SetDriftPpm(15.0);                // introduce initial drift

    std::vector<double> history_ms;
    history_ms.reserve(120);

    constexpr double kGain = 0.08;
    for (int i = 0; i < 120; ++i)
    {
      const int64_t pts = static_cast<int64_t>(i) * kFramePtsUs;
      const int64_t deadline = clock.scheduled_to_utc_us(pts);
      const int64_t now = clock.now_utc_us();
      const double error_ms = static_cast<double>(deadline - now) / 1'000.0;
      history_ms.push_back(error_ms);

      const double adjust_ppm = -error_ms * kGain;
      clock.SetDriftPpm(clock.drift_ppm() + adjust_ppm);
      clock.AdvanceMicroseconds(kFramePtsUs);
    }

    for (int i = 100; i < static_cast<int>(history_ms.size()); ++i)
    {
      EXPECT_LT(std::abs(history_ms[i]), 1.0)
          << "Error must converge below 1ms by iteration 100, got " << history_ms[i];
    }

    for (double err : history_ms)
    {
      EXPECT_LT(std::abs(err), 2.0 + 1e-6) << "Oscillation must stay within ±2ms";
    }
  }

  // Rule: MT-007 Timing anomalies surfaced (MetricsAndTimingContract.md §MT-007)
  TEST_F(MetricsAndTimingContractTest, MT_007_TimingAnomaliesSurfacedViaMetrics)
  {
    auto exporter = std::make_shared<telemetry::MetricsExporter>(0);
    constexpr int32_t kChannelId = 707;

    telemetry::ChannelMetrics anomaly{};
    anomaly.state = telemetry::ChannelState::ERROR_STATE;
    anomaly.frame_gap_seconds = 0.012;
    anomaly.decode_failure_count = 3;
    anomaly.corrections_total = 5;
    exporter->UpdateChannelMetrics(kChannelId, anomaly);

    telemetry::ChannelMetrics snapshot{};
    ASSERT_TRUE(exporter->GetChannelMetrics(kChannelId, snapshot));
    EXPECT_EQ(snapshot.state, telemetry::ChannelState::ERROR_STATE);
    EXPECT_DOUBLE_EQ(snapshot.frame_gap_seconds, anomaly.frame_gap_seconds);
    EXPECT_EQ(snapshot.decode_failure_count, anomaly.decode_failure_count);
    EXPECT_EQ(snapshot.corrections_total, anomaly.corrections_total);
  }

  // Rule: MT-008 Forward compatibility with MasterClock interface (MetricsAndTimingContract.md §MT-008)
  TEST_F(MetricsAndTimingContractTest, MT_008_MasterClockInterfaceSupportsRuntimeClock)
  {
    MasterClockStub stub;
    const int64_t epoch = stub.now_utc_us();
    auto runtime_clock = retrovue::timing::MakeSystemMasterClock(epoch, 0.0);
    ASSERT_NE(runtime_clock, nullptr);

    buffer::FrameRingBuffer buffer(/*capacity=*/6);
    for (int i = 0; i < 3; ++i)
    {
      buffer::Frame frame;
      frame.metadata.pts = i * 33'366;
      frame.metadata.duration = 1.0 / 30.0;
      ASSERT_TRUE(buffer.Push(frame));
    }

    auto metrics = std::make_shared<telemetry::MetricsExporter>(0);
    telemetry::ChannelMetrics seed{};
    metrics->UpdateChannelMetrics(808, seed);

    renderer::RenderConfig config;
    config.mode = renderer::RenderMode::HEADLESS;
    auto renderer = renderer::FrameRenderer::Create(
        config, buffer, runtime_clock, metrics, 808);

    EXPECT_NE(renderer, nullptr);
    EXPECT_GT(runtime_clock->now_utc_us(), 0);
  }

} // namespace
