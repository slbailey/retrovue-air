#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "retrovue/buffer/FrameRingBuffer.h"
#include "timing/TestMasterClock.h"

namespace retrovue::tests::integration
{
namespace
{

constexpr int64_t kPtsStepUs = 33'366;  // ~29.97 fps
constexpr int kTotalFrames = 18'000;    // 10 minutes of 30 fps content

double ComputeMeanAbs(const std::vector<double>& values)
{
  if (values.empty())
  {
    return 0.0;
  }
  const double sum = std::accumulate(values.begin(), values.end(), 0.0);
  return sum / static_cast<double>(values.size());
}

double ComputeP95(std::vector<double> values)
{
  if (values.empty())
  {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double rank = 0.95 * static_cast<double>(values.size() - 1);
  const auto lower_index = static_cast<size_t>(std::floor(rank));
  const auto upper_index = static_cast<size_t>(std::ceil(rank));
  const double fraction = rank - static_cast<double>(lower_index);
  return values[lower_index] +
         (values[upper_index] - values[lower_index]) * fraction;
}

class StubFrameProducer
{
public:
  explicit StubFrameProducer(int64_t pts_step_us)
      : pts_step_us_(pts_step_us),
        pts_counter_(0)
  {
  }

  bool Produce(buffer::FrameRingBuffer& buffer)
  {
    buffer::Frame frame;
    frame.metadata.pts = pts_counter_;
    frame.metadata.dts = pts_counter_;
    frame.metadata.duration =
        static_cast<double>(pts_step_us_) / 1'000'000.0;
    frame.metadata.asset_uri = "integration://cadence/stub";
    frame.width = 1920;
    frame.height = 1080;

    pts_counter_ += pts_step_us_;
    return buffer.Push(frame);
  }

private:
  const int64_t pts_step_us_;
  int64_t pts_counter_;
};

TEST(FrameCadenceIntegration, MaintainsCadenceOverTenMinutes)
{
  buffer::FrameRingBuffer buffer(512);
  StubFrameProducer producer(kPtsStepUs);

  auto clock = std::make_shared<retrovue::timing::TestMasterClock>();
  const int64_t epoch = 1'700'000'400'000'000;
  clock->SetEpochUtcUs(epoch);
  clock->SetRatePpm(0.0);
  clock->SetDriftPpm(0.0);
  clock->SetNow(epoch + 8'000, 0.0);  // introduce initial skew

  std::vector<double> abs_gaps_ms;
  abs_gaps_ms.reserve(kTotalFrames);
  uint64_t corrections_total = 0;

  for (int frame_index = 0; frame_index < kTotalFrames; ++frame_index)
  {
    ASSERT_TRUE(producer.Produce(buffer));

    buffer::Frame frame;
    ASSERT_TRUE(buffer.Pop(frame));

    const int64_t deadline = clock->scheduled_to_utc_us(frame.metadata.pts);
    const int64_t now = clock->now_utc_us();
    const int64_t gap_us = deadline - now;

    abs_gaps_ms.push_back(std::abs(static_cast<double>(gap_us) / 1'000.0));

    if (gap_us > 0)
    {
      // MC-003: pace rendering against MasterClock without wall sleeps.
      clock->AdvanceMicroseconds(gap_us);
    }
    else if (gap_us < 0)
    {
      // MC-003: apply feedback to converge scheduling error.
      ++corrections_total;
      const double adjust_ppm = std::clamp(
          -static_cast<double>(gap_us) / 1'000.0 * 0.05, -40.0, 40.0);
      clock->SetDriftPpm(clock->drift_ppm() + adjust_ppm);
      const int64_t catchup_us =
          std::min<int64_t>(kPtsStepUs, -gap_us);
      clock->AdvanceMicroseconds(catchup_us);
    }

    // Advance simulated time by the nominal frame period.
    clock->AdvanceMicroseconds(kPtsStepUs);
  }

  const double mean_abs_gap_ms = ComputeMeanAbs(abs_gaps_ms);
  const double p95_gap_ms = ComputeP95(abs_gaps_ms);

  EXPECT_LT(mean_abs_gap_ms, 10.0) << "Mean absolute gap should remain under 10 ms";
  EXPECT_LT(p95_gap_ms, 4.0) << "p95 absolute gap should remain under 4 ms";
  EXPECT_LE(corrections_total, 600u) << "MC-003: corrections should remain bounded";
}

}  // namespace
}  // namespace retrovue::tests::integration


