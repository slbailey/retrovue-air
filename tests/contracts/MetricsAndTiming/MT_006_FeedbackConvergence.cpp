#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "retrovue/timing/TestMasterClock.h"

#include <gtest/gtest.h>

namespace retrovue::tests {

namespace {
constexpr double kPpmDenom = 1'000'000.0;
constexpr int64_t kFramePtsUs = 33'366;  // ~29.97 fps
}  // namespace

TEST(MetricsAndTimingFeedbackSuite, MT_006_FeedbackConvergence) {
  timing::TestMasterClock clock;
  clock.SetEpochUtcUs(1'700'000'000'000'000);
  clock.SetRatePpm(0.0);
  clock.SetNow(clock.now_utc_us(), 0.0);

  double drift_ppm = (20.0 / (kFramePtsUs / 1000.0)) * 1000.0;
  clock.SetDriftPpm(drift_ppm);

  std::vector<double> history_ms;
  history_ms.reserve(120);

  for (int i = 0; i < 120; ++i) {
    const int64_t pts = static_cast<int64_t>(i) * kFramePtsUs;
    const int64_t deadline = clock.scheduled_to_utc_us(pts);
    const int64_t now = clock.now_utc_us();
    const double error_ms = static_cast<double>(deadline - now) / 1000.0;
    history_ms.push_back(error_ms);

    const double adjust_ppm = -error_ms / 2.0;
    clock.SetDriftPpm(clock.drift_ppm() + adjust_ppm);
    clock.AdvanceMicroseconds(kFramePtsUs);
  }

  for (int i = 100; i < static_cast<int>(history_ms.size()); ++i) {
    EXPECT_LT(std::abs(history_ms[i]), 1.0)
        << "Error must converge below 1ms by iteration 100, got " << history_ms[i];
  }

  for (double err : history_ms) {
    EXPECT_LT(std::abs(err), 2.0 + 1e-6) << "Oscillation must stay within ±2ms";
  }

  std::cout << "[MT_006] iterations=" << history_ms.size()
            << " final_error_ms=" << history_ms.back()
            << " drift_ppm=" << clock.drift_ppm() << std::endl;
}

}  // namespace retrovue::tests


