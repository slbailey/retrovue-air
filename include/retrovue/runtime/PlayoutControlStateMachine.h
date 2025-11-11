#ifndef RETROVUE_RUNTIME_PLAYOUT_CONTROL_STATE_MACHINE_H_
#define RETROVUE_RUNTIME_PLAYOUT_CONTROL_STATE_MACHINE_H_

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mutex>

#include "retrovue/runtime/OrchestrationLoop.h"

namespace retrovue::runtime {

class PlayoutControlStateMachine {
 public:
  enum class State {
    kIdle = 0,
    kBuffering = 1,
    kReady = 2,
    kPlaying = 3,
    kPaused = 4,
    kStopping = 5,
    kError = 6,
  };

  struct MetricsSnapshot {
    std::map<std::pair<State, State>, uint64_t> transitions;
    uint64_t illegal_transition_total = 0;
    uint64_t latency_violation_total = 0;
    uint64_t timeout_total = 0;
    uint64_t queue_overflow_total = 0;
    uint64_t recover_total = 0;
    uint64_t consistency_failure_total = 0;
    uint64_t late_seek_total = 0;
    double pause_latency_p95_ms = 0.0;
    double resume_latency_p95_ms = 0.0;
    double seek_latency_p95_ms = 0.0;
    double stop_latency_p95_ms = 0.0;
    double pause_deviation_p95_ms = 0.0;
    double last_pause_latency_ms = 0.0;
    double last_resume_latency_ms = 0.0;
    double last_seek_latency_ms = 0.0;
    double last_stop_latency_ms = 0.0;
    double last_pause_deviation_ms = 0.0;
    State state = State::kIdle;
  };

  PlayoutControlStateMachine();

  PlayoutControlStateMachine(const PlayoutControlStateMachine&) = delete;
  PlayoutControlStateMachine& operator=(const PlayoutControlStateMachine&) = delete;

  bool BeginSession(const std::string& command_id, int64_t request_utc_us);
  bool Pause(const std::string& command_id,
             int64_t request_utc_us,
             int64_t effective_utc_us,
             double boundary_deviation_ms);
  bool Resume(const std::string& command_id,
              int64_t request_utc_us,
              int64_t effective_utc_us);
  bool Seek(const std::string& command_id,
            int64_t request_utc_us,
            int64_t target_pts_us,
            int64_t effective_utc_us);
  bool Stop(const std::string& command_id,
            int64_t request_utc_us,
            int64_t effective_utc_us);
  bool Recover(const std::string& command_id, int64_t request_utc_us);

  void OnBufferDepth(std::size_t depth,
                     std::size_t capacity,
                     int64_t event_utc_us);
  void OnBackPressureEvent(OrchestrationLoop::BackPressureEvent event,
                           int64_t event_utc_us);
  void OnBackPressureCleared(int64_t event_utc_us);

  void OnExternalTimeout(int64_t event_utc_us);
  void OnQueueOverflow();

  [[nodiscard]] State state() const;
  [[nodiscard]] MetricsSnapshot Snapshot() const;

 private:
  void TransitionLocked(State to, int64_t event_utc_us);
  void RecordTransitionLocked(State from, State to);
  void RecordLatencyLocked(std::vector<double>& samples, double value_ms);
  double PercentileLocked(const std::vector<double>& samples, double percentile) const;
  bool RegisterCommandLocked(const std::string& command_id);
  void RecordIllegalTransitionLocked(State from, State attempted_to);

  constexpr static double kPauseLatencyThresholdMs = 33.0;
  constexpr static double kResumeLatencyThresholdMs = 50.0;
  constexpr static double kSeekLatencyThresholdMs = 250.0;
  constexpr static double kStopLatencyThresholdMs = 500.0;
  constexpr static std::size_t kReadinessThresholdFrames = 3;

  mutable std::mutex mutex_;

  State state_;
  std::unordered_map<std::string, int64_t> processed_commands_;
  int64_t current_pts_us_;
  std::map<std::pair<State, State>, uint64_t> transitions_;
  uint64_t illegal_transition_total_;
  uint64_t latency_violation_total_;
  uint64_t timeout_total_;
  uint64_t queue_overflow_total_;
  uint64_t recover_total_;
  uint64_t consistency_failure_total_;
  uint64_t late_seek_total_;
  std::vector<double> pause_latencies_ms_;
  std::vector<double> resume_latencies_ms_;
  std::vector<double> seek_latencies_ms_;
  std::vector<double> stop_latencies_ms_;
  std::vector<double> pause_deviation_ms_;
};

}  // namespace retrovue::runtime

#endif  // RETROVUE_RUNTIME_PLAYOUT_CONTROL_STATE_MACHINE_H_

