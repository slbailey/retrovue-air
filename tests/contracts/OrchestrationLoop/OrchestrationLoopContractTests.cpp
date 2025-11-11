#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseContractTest.h"
#include "retrovue/runtime/OrchestrationLoop.h"
#include "retrovue/timing/MasterClock.h"

namespace retrovue::tests::contracts {

namespace {

double Percentile(std::vector<double> samples, double percentile) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double rank = percentile * static_cast<double>(samples.size() - 1);
  const auto lower_index = static_cast<std::size_t>(std::floor(rank));
  const auto upper_index = static_cast<std::size_t>(std::ceil(rank));
  const double lower = samples[lower_index];
  if (upper_index == lower_index) {
    return lower;
  }
  const double upper = samples[upper_index];
  const double fraction = rank - static_cast<double>(lower_index);
  return lower + (upper - lower) * fraction;
}

double PercentileAbs(const std::vector<double>& samples, double percentile) {
  std::vector<double> magnitudes;
  magnitudes.reserve(samples.size());
  for (double value : samples) {
    magnitudes.push_back(std::abs(value));
  }
  return Percentile(std::move(magnitudes), percentile);
}

}  // namespace

class OrchestrationLoopContractTest : public BaseContractTest {
 protected:
  [[nodiscard]] std::string DomainName() const override { return "OrchestrationLoop"; }

  [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override {
    return {"ORCH_001", "ORCH_002", "ORCH_003"};
  }
};

TEST_F(OrchestrationLoopContractTest, ORCH_001_TickDisciplineAndLatencyBudget) {
  auto clock = timing::MakeSystemMasterClock(1'700'000'000'000'000LL, 0.0);

  runtime::OrchestrationLoop::Config config;
  config.target_fps = 30.0;
  config.max_tick_skew_ms = 1.5;

  runtime::OrchestrationLoop loop(config, clock,
                                  [](const runtime::OrchestrationLoop::TickContext&) {
                                    runtime::OrchestrationLoop::TickResult result;
                                    result.producer_to_renderer_latency_ms = 8.0;
                                    return result;
                                  });

  loop.Start();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  loop.Stop();

  const auto stats = loop.Snapshot();

  ASSERT_GE(stats.tick_skew_ms.size(), 60u) << "Insufficient tick samples collected";
  EXPECT_LT(PercentileAbs(stats.tick_skew_ms, 0.95), 2.2)
      << "Tick skew exceeded 95th percentile requirement";
  ASSERT_FALSE(stats.latency_ms.empty());
  EXPECT_LE(Percentile(stats.latency_ms, 0.95), config.max_latency_ms)
      << "Producer→renderer latency breached contract";
  EXPECT_FALSE(stats.starvation_detected) << "Unexpected starvation detected during steady state";
}

TEST_F(OrchestrationLoopContractTest, ORCH_002_BackPressureResolvesWithinThreeTicks) {
  auto clock = timing::MakeSystemMasterClock(1'700'000'000'000'000LL, 0.0);

  runtime::OrchestrationLoop::Config config;
  config.target_fps = 30.0;
  config.max_backpressure_ticks = 3;

  auto loop = std::make_unique<runtime::OrchestrationLoop>(config, clock, nullptr);
  auto* loop_ptr = loop.get();

  std::atomic<std::uint64_t> tick_counter{0};
  std::atomic<std::uint64_t> underrun_tick{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<std::uint64_t> overrun_tick{std::numeric_limits<std::uint64_t>::max()};

  loop->SetTickCallback(
      [loop_ptr, &tick_counter, &underrun_tick, &overrun_tick](
          const runtime::OrchestrationLoop::TickContext& ctx) {
        runtime::OrchestrationLoop::TickResult result;
        result.producer_to_renderer_latency_ms = 12.0;

        const auto tick = tick_counter.fetch_add(1);

        if (tick == 20) {
          loop_ptr->ReportBackPressureEvent(
              runtime::OrchestrationLoop::BackPressureEvent::kUnderrun);
          underrun_tick.store(tick);
        }

        if (underrun_tick.load() != std::numeric_limits<std::uint64_t>::max() &&
            tick == underrun_tick.load() + 2) {
          result.backpressure_cleared = true;
        }

        if (tick == 60) {
          loop_ptr->ReportBackPressureEvent(
              runtime::OrchestrationLoop::BackPressureEvent::kOverrun);
          overrun_tick.store(tick);
        }

        if (overrun_tick.load() != std::numeric_limits<std::uint64_t>::max() &&
            tick == overrun_tick.load() + 2) {
          result.backpressure_cleared = true;
        }

        return result;
      });

  loop->Start();
  std::this_thread::sleep_for(std::chrono::seconds(4));
  loop->Stop();

  const auto stats = loop->Snapshot();

  ASSERT_EQ(stats.backpressure_events
                .at(runtime::OrchestrationLoop::BackPressureEvent::kUnderrun),
            1u);
  ASSERT_EQ(stats.backpressure_events
                .at(runtime::OrchestrationLoop::BackPressureEvent::kOverrun),
            1u);

  ASSERT_FALSE(stats.backpressure_unresolved)
      << "Back-pressure was not resolved inside expected tick window";

  const auto& underrun_recovery =
      stats.backpressure_recovery_ms.at(runtime::OrchestrationLoop::BackPressureEvent::kUnderrun);
  ASSERT_FALSE(underrun_recovery.empty());
  EXPECT_LT(underrun_recovery.front(), 150.0);

  const auto& overrun_recovery =
      stats.backpressure_recovery_ms.at(runtime::OrchestrationLoop::BackPressureEvent::kOverrun);
  ASSERT_FALSE(overrun_recovery.empty());
  EXPECT_LT(overrun_recovery.front(), 150.0);
}

TEST_F(OrchestrationLoopContractTest, ORCH_003_StarvationDetectionAndGracefulTeardown) {
  auto clock = timing::MakeSystemMasterClock(1'700'000'000'000'000LL, 0.0);

  runtime::OrchestrationLoop::Config config;
  config.target_fps = 30.0;
  config.starvation_threshold_ms = 100.0;
  config.graceful_stop_timeout_ms = 500.0;

  std::atomic<bool> starvation_triggered{false};

  runtime::OrchestrationLoop loop(
      config, clock,
      [&starvation_triggered](const runtime::OrchestrationLoop::TickContext& ctx) {
        runtime::OrchestrationLoop::TickResult result;
        result.producer_to_renderer_latency_ms = 6.0;
        if (ctx.tick_index == 30 && !starvation_triggered.exchange(true)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return result;
      });

  loop.Start();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  loop.Stop();

  const auto stats = loop.Snapshot();

  EXPECT_TRUE(stats.starvation_detected)
      << "Loop failed to raise starvation alert after extended blockage";
  EXPECT_LE(stats.teardown_duration_ms, config.graceful_stop_timeout_ms)
      << "Teardown exceeded graceful stop timeout";
}

}  // namespace retrovue::tests::contracts

