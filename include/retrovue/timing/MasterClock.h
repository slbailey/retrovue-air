#ifndef RETROVUE_TIMING_MASTER_CLOCK_H_
#define RETROVUE_TIMING_MASTER_CLOCK_H_

#include <cstdint>
#include <memory>
#include <thread>
#include <chrono>

namespace retrovue::timing {

// MasterClock provides monotonic and wall-clock time along with PTS to UTC mapping.
class MasterClock {
 public:
  virtual ~MasterClock() = default;

  // Returns current UTC time in microseconds since Unix epoch.
  virtual int64_t now_utc_us() const = 0;

  // Returns current monotonic time in seconds relative to clock start.
  virtual double now_monotonic_s() const = 0;

  // Maps a presentation timestamp (in microseconds) to an absolute UTC deadline.
  virtual int64_t scheduled_to_utc_us(int64_t pts_us) const = 0;

  // Reports measured drift in parts per million relative to upstream reference.
  virtual double drift_ppm() const = 0;

  // Returns true if this is a fake/test clock (for testing only).
  // Fake clocks should not trigger real-time sleeps in consumers.
  virtual bool is_fake() const { return false; }

  // Blocks until the clock reaches or exceeds target_utc_us.
  // For real clocks, this uses sleep-based waiting.
  // For fake clocks, this blocks on a condition variable that is woken by advance_us().
  // This method respects stop_requested_ patterns in consumers by checking periodically.
  virtual void WaitUntilUtcUs(int64_t target_utc_us) const {
    // Default implementation: sleep-based waiting
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
};

std::shared_ptr<MasterClock> MakeSystemMasterClock(int64_t epoch_utc_us, double rate_ppm);
}  // namespace retrovue::timing

#endif  // RETROVUE_TIMING_MASTER_CLOCK_H_
