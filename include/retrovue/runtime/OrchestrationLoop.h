#ifndef RETROVUE_RUNTIME_ORCHESTRATION_LOOP_H_
#define RETROVUE_RUNTIME_ORCHESTRATION_LOOP_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "retrovue/timing/MasterClock.h"

namespace retrovue::runtime {

class OrchestrationLoop {
 public:
  enum class BackPressureEvent {
    kUnderrun,
    kOverrun,
    kStall,
  };

  struct Config {
    double target_fps = 30.0;
    double max_tick_skew_ms = 1.0;
    double max_latency_ms = 33.0;
    std::size_t max_backpressure_ticks = 3;
    double starvation_threshold_ms = 100.0;
    double graceful_stop_timeout_ms = 500.0;
  };

  struct TickContext {
    std::uint64_t tick_index;
    int64_t scheduled_deadline_utc_us;
  };

  struct TickResult {
    std::optional<double> producer_to_renderer_latency_ms;
    bool deadline_met = true;
    bool backpressure_cleared = false;
  };

  struct Stats {
    std::vector<double> tick_skew_ms;
    std::vector<double> latency_ms;
    std::map<BackPressureEvent, std::size_t> backpressure_events;
    std::map<BackPressureEvent, std::vector<double>> backpressure_recovery_ms;
    bool starvation_detected = false;
    bool backpressure_unresolved = false;
    double teardown_duration_ms = 0.0;
  };

  using TickCallback = std::function<TickResult(const TickContext&)>;

  OrchestrationLoop(Config config,
                    std::shared_ptr<timing::MasterClock> clock,
                    TickCallback callback);
  ~OrchestrationLoop();

  OrchestrationLoop(const OrchestrationLoop&) = delete;
  OrchestrationLoop& operator=(const OrchestrationLoop&) = delete;

  void Start();
  void Stop();

  void SetTickCallback(TickCallback callback);

  void ReportBackPressureEvent(BackPressureEvent event);

  [[nodiscard]] Stats Snapshot() const;

 private:
  struct PendingBackPressure {
    BackPressureEvent event;
    std::uint64_t start_tick;
    int64_t start_time_utc_us;
    std::size_t ticks_elapsed;
  };

  void Run();
  void RecordTickSkew(double skew_ms);
  void RecordLatency(double latency_ms);
  void HandleBackPressure(const TickResult& result);
  static double ComputePercentile(const std::vector<double>& values, double percentile);

  Config config_;
  std::shared_ptr<timing::MasterClock> clock_;
  TickCallback tick_callback_;

  std::atomic<bool> running_;
  std::atomic<std::uint64_t> tick_index_;
  std::unique_ptr<std::thread> thread_;

  mutable std::mutex metrics_mutex_;
  Stats stats_;
  std::optional<PendingBackPressure> pending_backpressure_;
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace retrovue::runtime

#endif  // RETROVUE_RUNTIME_ORCHESTRATION_LOOP_H_

