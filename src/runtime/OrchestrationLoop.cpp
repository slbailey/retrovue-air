#include "retrovue/runtime/OrchestrationLoop.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace retrovue::runtime {

namespace {

constexpr double kMicrosecondsPerSecond = 1'000'000.0;

int64_t ToMicroseconds(double seconds) {
  return static_cast<int64_t>(std::llround(seconds * kMicrosecondsPerSecond));
}

double ToMilliseconds(int64_t microseconds) {
  return static_cast<double>(microseconds) / 1'000.0;
}

}  // namespace

OrchestrationLoop::OrchestrationLoop(Config config,
                                     std::shared_ptr<timing::MasterClock> clock,
                                     TickCallback callback)
    : config_(std::move(config)),
      clock_(std::move(clock)),
      tick_callback_(std::move(callback)),
      running_(false),
      tick_index_(0) {}

OrchestrationLoop::~OrchestrationLoop() { Stop(); }

void OrchestrationLoop::Start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return;
  }

  start_time_ = std::chrono::steady_clock::now();
  thread_ = std::make_unique<std::thread>(&OrchestrationLoop::Run, this);
}

void OrchestrationLoop::Stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    return;
  }

  const auto stop_start = std::chrono::steady_clock::now();
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
  const auto stop_end = std::chrono::steady_clock::now();
  const double duration_ms =
      std::chrono::duration<double, std::milli>(stop_end - stop_start).count();

  std::lock_guard<std::mutex> lock(metrics_mutex_);
  stats_.teardown_duration_ms = duration_ms;
}

void OrchestrationLoop::SetTickCallback(TickCallback callback) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  tick_callback_ = std::move(callback);
}

void OrchestrationLoop::ReportBackPressureEvent(BackPressureEvent event) {
  const auto now_utc =
      clock_ ? clock_->now_utc_us()
             : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());

  std::lock_guard<std::mutex> lock(metrics_mutex_);
  stats_.backpressure_events[event]++;
  pending_backpressure_ = PendingBackPressure{
      .event = event,
      .start_tick = tick_index_.load(std::memory_order_acquire),
      .start_time_utc_us = now_utc,
      .ticks_elapsed = 0,
  };
}

OrchestrationLoop::Stats OrchestrationLoop::Snapshot() const {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  return stats_;
}

void OrchestrationLoop::Run() {
  const double interval_seconds = 1.0 / std::max(config_.target_fps, 1.0);
  const int64_t interval_us = ToMicroseconds(interval_seconds);

  int64_t next_deadline =
      clock_ ? clock_->now_utc_us() + interval_us
             : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count()) +
                   interval_us;

  auto previous_tick_start = std::chrono::steady_clock::now();

  while (running_.load(std::memory_order_acquire)) {
    while (running_.load(std::memory_order_acquire)) {
      const int64_t now_utc = clock_
                                  ? clock_->now_utc_us()
                                  : static_cast<int64_t>(
                                        std::chrono::duration_cast<std::chrono::microseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
      const int64_t remaining_us = next_deadline - now_utc;
      if (remaining_us <= 0) {
        break;
      }

      const int64_t sleep_us = std::min<int64_t>(remaining_us, 5'000);
      std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }

    const auto tick_start = std::chrono::steady_clock::now();
    const double gap_ms =
        std::chrono::duration<double, std::milli>(tick_start - previous_tick_start).count();
    previous_tick_start = tick_start;
    if (gap_ms > config_.starvation_threshold_ms) {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      stats_.starvation_detected = true;
    }

    const int64_t actual_utc =
        clock_ ? clock_->now_utc_us()
               : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const double skew_ms = ToMilliseconds(actual_utc - next_deadline);
    RecordTickSkew(skew_ms);

    TickResult result;
    if (tick_callback_) {
      TickContext context{tick_index_.load(std::memory_order_relaxed), next_deadline};
      result = tick_callback_(context);
    }

    if (!result.deadline_met) {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      stats_.backpressure_unresolved = true;
    }

    if (result.producer_to_renderer_latency_ms) {
      RecordLatency(*result.producer_to_renderer_latency_ms);
    }

    HandleBackPressure(result);

    tick_index_.fetch_add(1, std::memory_order_acq_rel);
    next_deadline += interval_us;
    if (actual_utc - next_deadline > interval_us) {
      next_deadline = actual_utc + interval_us;
    }
  }
}

void OrchestrationLoop::RecordTickSkew(double skew_ms) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  stats_.tick_skew_ms.push_back(skew_ms);
}

void OrchestrationLoop::RecordLatency(double latency_ms) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  stats_.latency_ms.push_back(latency_ms);
}

void OrchestrationLoop::HandleBackPressure(const TickResult& result) {
  std::lock_guard<std::mutex> lock(metrics_mutex_);
  if (!pending_backpressure_) {
    return;
  }

  pending_backpressure_->ticks_elapsed++;

  if (result.backpressure_cleared) {
    const int64_t now_utc =
        clock_ ? clock_->now_utc_us()
               : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
    const double recovery_ms =
        ToMilliseconds(now_utc - pending_backpressure_->start_time_utc_us);
    stats_.backpressure_recovery_ms[pending_backpressure_->event].push_back(recovery_ms);
    pending_backpressure_.reset();
    return;
  }

  if (pending_backpressure_->ticks_elapsed > config_.max_backpressure_ticks) {
    stats_.backpressure_unresolved = true;
  }
}

double OrchestrationLoop::ComputePercentile(const std::vector<double>& values,
                                            double percentile) {
  if (values.empty()) {
    return 0.0;
  }

  std::vector<double> copy = values;
  const double rank = percentile * static_cast<double>(copy.size() - 1);
  const std::size_t lower_index = static_cast<std::size_t>(std::floor(rank));
  const std::size_t upper_index = static_cast<std::size_t>(std::ceil(rank));
  std::nth_element(copy.begin(), copy.begin() + lower_index, copy.end());
  const double lower = copy[lower_index];
  if (upper_index == lower_index) {
    return lower;
  }
  std::nth_element(copy.begin(), copy.begin() + upper_index, copy.end());
  const double upper = copy[upper_index];
  const double fraction = rank - static_cast<double>(lower_index);
  return lower + (upper - lower) * fraction;
}

}  // namespace retrovue::runtime

