#include "timing/TestMasterClock.h"

#include <cmath>
#include <thread>
#include <chrono>

namespace retrovue::timing {

namespace {
constexpr double kMillion = 1'000'000.0;
}

TestMasterClock::TestMasterClock(Mode mode)
    : mode_(mode),
      utc_us_(0),
      monotonic_s_(0.0),
      epoch_utc_us_(0),
      rate_ppm_(0.0),
      drift_ppm_(0.0),
      max_wait_us_(0) {}

TestMasterClock::TestMasterClock(int64_t start_time_us, Mode mode)
    : mode_(mode),
      utc_us_(start_time_us),
      monotonic_s_(static_cast<double>(start_time_us) / kMillion),
      epoch_utc_us_(0),
      rate_ppm_(0.0),
      drift_ppm_(0.0),
      max_wait_us_(0) {}

int64_t TestMasterClock::now_utc_us() const {
  return utc_us_.load(std::memory_order_acquire);
}

double TestMasterClock::now_monotonic_s() const {
  if (mode_ == Mode::Deterministic) {
    std::lock_guard<std::mutex> lock(mutex_);
    return monotonic_s_;
  }
  return monotonic_s_;
}

int64_t TestMasterClock::scheduled_to_utc_us(int64_t pts_us) const {
  const long double scale =
      1.0L + (static_cast<long double>(rate_ppm_) + static_cast<long double>(drift_ppm_)) /
                 kMillion;
  const long double adjusted = static_cast<long double>(pts_us) * scale;
  const auto rounded = static_cast<int64_t>(std::llround(adjusted));
  return epoch_utc_us_ + rounded;
}

double TestMasterClock::drift_ppm() const { return drift_ppm_; }

bool TestMasterClock::is_fake() const {
  return mode_ == Mode::Deterministic;
}

void TestMasterClock::WaitUntilUtcUs(int64_t target_utc_us) const {
  if (mode_ == Mode::Deterministic) {
    // Deterministic mode: block on condition variable
    std::unique_lock<std::mutex> lock(mutex_);
    if (max_wait_us_ > 0) {
      // Use timeout to prevent deadlocks
      cv_.wait_for(lock, std::chrono::microseconds(max_wait_us_), [&] {
        return utc_us_.load(std::memory_order_acquire) >= target_utc_us;
      });
    } else {
      // Wait indefinitely (test must advance time)
      cv_.wait(lock, [&] {
        return utc_us_.load(std::memory_order_acquire) >= target_utc_us;
      });
    }
  } else {
    // RealTime mode: use sleep-based waiting (existing behavior)
    while (true) {
      const int64_t now = now_utc_us();
      const int64_t remaining = target_utc_us - now;
      if (remaining <= 0) {
        break;
      }
      const int64_t sleep_us = (remaining > 2'000) ? remaining - 1'000
                                                    : std::max<int64_t>(remaining / 2, 200);
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
  }
}

void TestMasterClock::SetNow(int64_t utc_us, double monotonic_s) {
  utc_us_.store(utc_us, std::memory_order_release);
  if (mode_ == Mode::Deterministic) {
    std::lock_guard<std::mutex> lock(mutex_);
    monotonic_s_ = monotonic_s;
    cv_.notify_all();
  } else {
    monotonic_s_ = monotonic_s;
  }
}

void TestMasterClock::set_time_us(int64_t time_us) {
  const double monotonic = static_cast<double>(time_us) / kMillion;
  SetNow(time_us, monotonic);
}

void TestMasterClock::AdvanceMicroseconds(int64_t delta_us) {
  utc_us_.fetch_add(delta_us, std::memory_order_acq_rel);
  const double delta_s = static_cast<double>(delta_us) / kMillion;
  
  if (mode_ == Mode::Deterministic) {
    // In deterministic mode, protect monotonic_s_ with mutex and notify waiters
    std::lock_guard<std::mutex> lock(mutex_);
    monotonic_s_ += delta_s;
    cv_.notify_all();
  } else {
    // In RealTime mode, no synchronization needed
    monotonic_s_ += delta_s;
  }
}

void TestMasterClock::AdvanceSeconds(double delta_s) {
  const int64_t delta_us = static_cast<int64_t>(std::llround(delta_s * kMillion));
  AdvanceMicroseconds(delta_us);
}

void TestMasterClock::SetDriftPpm(double ppm) { drift_ppm_ = ppm; }

void TestMasterClock::SetEpochUtcUs(int64_t epoch_utc_us) { epoch_utc_us_ = epoch_utc_us; }

void TestMasterClock::SetRatePpm(double rate_ppm) { rate_ppm_ = rate_ppm; }

void TestMasterClock::SetMaxWaitUs(int64_t max_wait_us) {
  max_wait_us_ = max_wait_us;
}

}  // namespace retrovue::timing


