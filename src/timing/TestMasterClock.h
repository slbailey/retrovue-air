#ifndef RETROVUE_TIMING_TEST_MASTER_CLOCK_H_
#define RETROVUE_TIMING_TEST_MASTER_CLOCK_H_

#include "retrovue/timing/MasterClock.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace retrovue::timing {

// TestMasterClock supports two modes:
// - RealTime: Uses sleep-based waiting (for tests that need real time progression)
// - Deterministic: Uses condition variable-based waiting (for fully deterministic tests)
class TestMasterClock : public MasterClock {
 public:
  enum class Mode {
    RealTime,        // Sleep-based waiting (existing behavior)
    Deterministic   // Condition variable-based waiting (no real sleeps)
  };

  // Construct with RealTime mode (default, backward compatible)
  explicit TestMasterClock(Mode mode = Mode::RealTime);

  // Construct with Deterministic mode and initial time
  explicit TestMasterClock(int64_t start_time_us, Mode mode = Mode::Deterministic);

  int64_t now_utc_us() const override;
  double now_monotonic_s() const override;
  int64_t scheduled_to_utc_us(int64_t pts_us) const override;
  double drift_ppm() const override;
  bool is_fake() const override;
  void WaitUntilUtcUs(int64_t target_utc_us) const override;

  // Time control methods
  void SetNow(int64_t utc_us, double monotonic_s);
  void AdvanceMicroseconds(int64_t delta_us);
  void AdvanceSeconds(double delta_s);
  void SetDriftPpm(double ppm);
  void SetEpochUtcUs(int64_t epoch_utc_us);
  void SetRatePpm(double rate_ppm);

  // Deterministic mode helpers (compatible with FakeMasterClock API)
  void advance_us(int64_t delta_us) { AdvanceMicroseconds(delta_us); }
  void set_time_us(int64_t time_us);
  int64_t get_time_us() const { return now_utc_us(); }

  // Set maximum wait timeout for deterministic mode (prevents deadlocks)
  // If set, WaitUntilUtcUs will timeout after max_wait_us even if time hasn't advanced
  void SetMaxWaitUs(int64_t max_wait_us);

 private:
  Mode mode_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::atomic<int64_t> utc_us_;
  double monotonic_s_;  // Protected by mutex_ in deterministic mode
  int64_t epoch_utc_us_;
  double rate_ppm_;
  double drift_ppm_;
  int64_t max_wait_us_;  // Maximum wait timeout for deterministic mode (0 = no timeout)
};

}  // namespace retrovue::timing

#endif  // RETROVUE_TIMING_TEST_MASTER_CLOCK_H_


