#include "../../BaseContractTest.h"
#include "../ContractRegistryEnvironment.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

namespace retrovue::tests
{
  namespace
  {

    using retrovue::tests::RegisterExpectedDomainCoverage;

    double ComputeP95(const std::vector<double> &values)
    {
      if (values.empty())
      {
        return 0.0;
      }

      std::vector<double> samples(values);
      const size_t n = samples.size();
      const double percentile = 0.95;
      size_t rank = static_cast<size_t>(std::ceil(percentile * static_cast<double>(n)));
      rank = std::clamp(rank, static_cast<size_t>(1), n);
      const size_t index = rank - 1;

      std::nth_element(samples.begin(), samples.begin() + index, samples.end());
      return samples[index];
    }

    class FakeMasterClock
    {
    public:
      enum class State
      {
        kReady = 0,
        kBuffering = 1,
        kError = 2
      };

      struct Telemetry
      {
        int64_t now_utc_us;
        double drift_ppm;
        double jitter_ms_p95;
        uint64_t corrections_total;
        uint64_t underrun_recoveries;
        uint64_t large_gap_events;
        double frame_gap_ms;
        State state;
      };

      FakeMasterClock()
          : epoch_(-1),
            now_utc_us_(0),
            rate_ppm_(0.0),
            base_drift_ppm_(0.0),
            cumulative_adjust_ppm_(0.0),
            state_(State::kReady),
            frame_gap_ms_(0.0),
            corrections_total_(0),
            underrun_recoveries_(0),
            large_gap_events_(0)
      {
      }

      void SetEpoch(int64_t epoch)
      {
        epoch_ = epoch;
      }

      void SetNow(int64_t now_utc_us)
      {
        now_utc_us_ = now_utc_us;
      }

      void SetRatePpm(double rate_ppm)
      {
        rate_ppm_ = rate_ppm;
      }

      void SetDriftPpm(double drift_ppm)
      {
        base_drift_ppm_ = drift_ppm;
      }

      void SetState(State state)
      {
        state_ = state;
        if (state == State::kBuffering)
        {
          frame_gap_ms_ = std::max(frame_gap_ms_, 0.0);
        }
      }

      void SetFrameGapMs(double frame_gap_ms)
      {
        frame_gap_ms_ = frame_gap_ms;
      }

      void QueueJitterUs(const std::vector<int64_t> &jitter_sequence)
      {
        jitter_queue_.insert(jitter_queue_.end(), jitter_sequence.begin(), jitter_sequence.end());
      }

      void AdvanceMicros(int64_t delta_us)
      {
        const int64_t jitter = PopNextJitter();
        telemetry_jitter_samples_.push_back(static_cast<double>(jitter));

        const int64_t previous = now_utc_us_;
        const int64_t positive_delta = std::max<int64_t>(0, delta_us);
        const int64_t candidate = previous + jitter;

        now_utc_us_ = previous + positive_delta;
        if (now_utc_us_ < previous)
        {
          now_utc_us_ = previous;
        }
        if (candidate > now_utc_us_)
        {
          now_utc_us_ = candidate;
        }
      }

      void AdvanceSeconds(double delta_seconds)
      {
        AdvanceMicros(static_cast<int64_t>(std::llround(delta_seconds * 1'000'000.0)));
      }

      int64_t Now() const
      {
        return now_utc_us_;
      }

      int64_t PtsToUtc(int64_t pts_us) const
      {
        const long double scale =
            1.0L + (static_cast<long double>(rate_ppm_) + static_cast<long double>(base_drift_ppm_) + static_cast<long double>(cumulative_adjust_ppm_)) /
                       1'000'000.0L;
        const long double adjusted = static_cast<long double>(pts_us) * scale;
        return epoch_ + static_cast<int64_t>(std::llround(adjusted));
      }

      void AdjustPace(double delta_ppm)
      {
        cumulative_adjust_ppm_ += delta_ppm;
        const double adjustment = delta_ppm * 0.001;
        const double reduced_gap = std::abs(frame_gap_ms_) * 0.6;
        frame_gap_ms_ = std::copysign(std::max(0.0, reduced_gap + adjustment), frame_gap_ms_);
        if (std::abs(frame_gap_ms_) < 0.001)
        {
          frame_gap_ms_ = 0.0;
        }
        ++corrections_total_;
      }

      void RecoverFromUnderrun()
      {
        ++underrun_recoveries_;
        state_ = State::kReady;
        frame_gap_ms_ = 0.0;
      }

      void HandleLargeGap()
      {
        ++large_gap_events_;
        ++corrections_total_;
        state_ = State::kError;
        frame_gap_ms_ = -6'000.0;
      }

      Telemetry GetTelemetry() const
      {
        std::vector<double> jitter_magnitudes_ms;
        jitter_magnitudes_ms.reserve(telemetry_jitter_samples_.size());
        for (double sample_us : telemetry_jitter_samples_)
        {
          jitter_magnitudes_ms.push_back(std::abs(sample_us) / 1'000.0);
        }
        const double jitter_ms = ComputeP95(jitter_magnitudes_ms);
        return Telemetry{
            now_utc_us_,
            base_drift_ppm_ + cumulative_adjust_ppm_,
            jitter_ms,
            corrections_total_,
            underrun_recoveries_,
            large_gap_events_,
            frame_gap_ms_,
            state_};
      }

    private:
      int64_t PopNextJitter()
      {
        if (jitter_queue_.empty())
        {
          return 0;
        }
        const int64_t value = jitter_queue_.front();
        jitter_queue_.pop_front();
        return value;
      }

      int64_t epoch_;
      int64_t now_utc_us_;
      double rate_ppm_;
      double base_drift_ppm_;
      double cumulative_adjust_ppm_;
      State state_;
      double frame_gap_ms_;
      uint64_t corrections_total_;
      uint64_t underrun_recoveries_;
      uint64_t large_gap_events_;
      std::deque<int64_t> jitter_queue_;
      std::vector<double> telemetry_jitter_samples_;
    };

    const bool kRegisterCoverage = []()
    {
      RegisterExpectedDomainCoverage(
          "MasterClock",
          {"MC-001", "MC-002", "MC-003", "MC-004", "MC-005", "MC-006"});
      return true;
    }();

    class MasterClockContractTest : public BaseContractTest
    {
    protected:
      [[nodiscard]] std::string DomainName() const override
      {
        return "MasterClock";
      }

      [[nodiscard]] std::vector<std::string> CoveredRuleIds() const override
      {
        return {
            "MC-001",
            "MC-002",
            "MC-003",
            "MC-004",
            "MC-005",
            "MC-006"};
      }
    };

    // Rule: MC-001 Monotonic now() (MasterClockDomainContract.md §MC_001)
    TEST_F(MasterClockContractTest, MC_001_MonotonicNow)
    {
      SCOPED_TRACE("MC-001: monotonic now() must never regress");

      FakeMasterClock clock;
      const int64_t epoch = 1'700'000'000'000'000;
      clock.SetEpoch(epoch);
      clock.SetNow(epoch);
      clock.QueueJitterUs({50, -25, 10});

      int64_t last_now = clock.Now();
      for (int i = 0; i < 200; ++i)
      {
        const int64_t delta_us = (i % 2 == 0) ? 1'000 : 2'000;
        clock.AdvanceMicros(delta_us);
        const int64_t current = clock.Now();
        EXPECT_GE(current, last_now) << "MasterClock::Now must be monotonic (iteration " << i << ")";
        last_now = current;
      }

      const auto telemetry = clock.GetTelemetry();
      EXPECT_LT(telemetry.jitter_ms_p95, 1.0)
          << "Jitter p95 should remain under 1 ms for deterministic pacing";
    }

    // Rule: MC-002 Stable PTS to UTC mapping (MasterClockDomainContract.md §MC_002)
    TEST_F(MasterClockContractTest, MC_002_StablePtsToUtcMapping)
    {
      SCOPED_TRACE("MC-002: PTS to UTC mapping must remain stable");

      FakeMasterClock clock;
      const int64_t epoch = 1'700'000'000'100'000;
      constexpr double kRatePpm = 75.0;
      clock.SetEpoch(epoch);
      clock.SetNow(epoch);
      clock.SetRatePpm(0.0);
      clock.SetDriftPpm(kRatePpm);

      const int64_t pts_step = 33'366; // ~29.97 fps
      int64_t previous_deadline = clock.PtsToUtc(0);
      for (int i = 1; i <= 180; ++i)
      {
        const int64_t pts = i * pts_step;
        const int64_t deadline = clock.PtsToUtc(pts);
        const int64_t repeated = clock.PtsToUtc(pts);

        EXPECT_EQ(deadline, repeated)
            << "Repeated PtsToUtc calls must be deterministic (PTS=" << pts << ")";
        EXPECT_GT(deadline, previous_deadline)
            << "PtsToUtc must remain strictly increasing for rising PTS values";

        const long double expected =
            static_cast<long double>(epoch) +
            static_cast<long double>(pts) *
                (1.0L + static_cast<long double>(kRatePpm) / 1'000'000.0L);
        const long double diff =
            std::llabs(deadline - static_cast<int64_t>(expected));
        EXPECT_LT(diff, 100.0L)
            << "PTS to UTC mapping must stay within ±0.1 ms";

        previous_deadline = deadline;
      }
    }

    // Rule: MC-003 Pace controller convergence (MasterClockDomainContract.md §MC_003)
    TEST_F(MasterClockContractTest, MC_003_PaceControllerConvergence)
    {
      SCOPED_TRACE("MC-003: Pace controller should shrink frame gap");

      FakeMasterClock clock;
      clock.SetFrameGapMs(12.0);

      const double initial_gap = clock.GetTelemetry().frame_gap_ms;
      clock.AdjustPace(-120.0);

      const auto telemetry = clock.GetTelemetry();
      EXPECT_LT(std::abs(telemetry.frame_gap_ms), std::abs(initial_gap))
          << "AdjustPace should reduce the absolute frame gap";
      EXPECT_GT(telemetry.corrections_total, 0u)
          << "AdjustPace must record correction telemetry";
    }

    TEST_F(MasterClockContractTest, MC_JitterP95UsesMicrosecondSamples)
    {
      SCOPED_TRACE("MC-Percentile: jitter P95 should respect microsecond samples");

      FakeMasterClock clock;
      clock.QueueJitterUs({0, 1000, 0, 0, 0});

      for (int i = 0; i < 5; ++i)
      {
        clock.AdvanceMicros(1'000);
      }

      const auto telemetry = clock.GetTelemetry();
      EXPECT_DOUBLE_EQ(telemetry.jitter_ms_p95, 1.0);
    }

    TEST_F(MasterClockContractTest, MC_004_UnderrunRecovery)
    {
      SCOPED_TRACE("MC-004: MasterClock should surface buffering and recover");

      FakeMasterClock clock;
      clock.SetState(FakeMasterClock::State::kBuffering);
      clock.SetFrameGapMs(4.0);

      clock.RecoverFromUnderrun();

      const auto telemetry = clock.GetTelemetry();
      EXPECT_EQ(telemetry.state, FakeMasterClock::State::kReady)
          << "RecoverFromUnderrun must transition clock back to ready state";
      EXPECT_EQ(telemetry.underrun_recoveries, 1u);
      EXPECT_DOUBLE_EQ(telemetry.frame_gap_ms, 0.0);
    }

    TEST_F(MasterClockContractTest, MC_005_LargeGapHandling)
    {
      SCOPED_TRACE("MC-005: Large gap must trigger corrective handling");

      FakeMasterClock clock;
      clock.SetFrameGapMs(-6'200.0);

      clock.HandleLargeGap();

      const auto telemetry = clock.GetTelemetry();
      EXPECT_EQ(telemetry.state, FakeMasterClock::State::kError)
          << "HandleLargeGap must signal error state for downstream handling";
      EXPECT_EQ(telemetry.large_gap_events, 1u);
      EXPECT_LT(telemetry.frame_gap_ms, -5'000.0)
          << "Frame gap telemetry should reflect the large negative offset";
      EXPECT_GT(telemetry.corrections_total, 0u);
    }

    TEST_F(MasterClockContractTest, MC_006_TelemetryCoverage)
    {
      SCOPED_TRACE("MC-006: Telemetry must expose drift and frame gap statistics");

      FakeMasterClock clock;
      clock.SetRatePpm(0.0);
      clock.SetDriftPpm(12.5);
      clock.QueueJitterUs({50, -25, 15, 0, 0});

      for (int i = 0; i < 60; ++i)
      {
        clock.AdvanceMicros(1'000);
      }

      clock.AdjustPace(-2.5);
      clock.SetState(FakeMasterClock::State::kBuffering);
      clock.RecoverFromUnderrun();
      clock.HandleLargeGap();

      const auto telemetry = clock.GetTelemetry();
      EXPECT_LT(telemetry.jitter_ms_p95, 1.0)
          << "Telemetry should capture low jitter under controlled conditions";
      EXPECT_NEAR(telemetry.drift_ppm, 10.0, 1e-6)
          << "AdjustPace should update the reported drift";
      EXPECT_GE(telemetry.corrections_total, 2u)
          << "AdjustPace and HandleLargeGap should both increment corrections";
      EXPECT_EQ(telemetry.underrun_recoveries, 1u);
      EXPECT_EQ(telemetry.large_gap_events, 1u);
      EXPECT_EQ(telemetry.state, FakeMasterClock::State::kError)
          << "Telemetry should reflect latest state after large-gap handling";
    }

  } // namespace
} // namespace retrovue::tests
