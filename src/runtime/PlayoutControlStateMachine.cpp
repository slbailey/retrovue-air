#include "retrovue/runtime/PlayoutControlStateMachine.h"

#include <algorithm>
#include <cmath>

namespace retrovue::runtime
{

  namespace
  {

    double MicrosecondsToMilliseconds(int64_t delta_us)
    {
      return static_cast<double>(delta_us) / 1'000.0;
    }

  } // namespace

  PlayoutControlStateMachine::PlayoutControlStateMachine()
      : state_(State::kIdle),
        current_pts_us_(0),
        illegal_transition_total_(0),
        latency_violation_total_(0),
        timeout_total_(0),
        queue_overflow_total_(0),
        recover_total_(0),
        consistency_failure_total_(0),
        late_seek_total_(0) {}

  bool PlayoutControlStateMachine::BeginSession(const std::string &command_id,
                                                int64_t request_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true; // Duplicate command is acknowledged.
    }

    if (state_ != State::kIdle)
    {
      RecordIllegalTransitionLocked(state_, State::kBuffering);
      return false;
    }

    TransitionLocked(State::kBuffering, request_utc_us);
    return true;
  }

  bool PlayoutControlStateMachine::Pause(const std::string &command_id,
                                         int64_t request_utc_us,
                                         int64_t effective_utc_us,
                                         double boundary_deviation_ms)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true;
    }

    if (state_ != State::kPlaying)
    {
      RecordIllegalTransitionLocked(state_, State::kPaused);
      return false;
    }

    const double latency_ms =
        MicrosecondsToMilliseconds(effective_utc_us - request_utc_us);
    RecordLatencyLocked(pause_latencies_ms_, latency_ms);
    pause_deviation_ms_.push_back(boundary_deviation_ms);

    if (latency_ms > kPauseLatencyThresholdMs)
    {
      ++latency_violation_total_;
    }

    TransitionLocked(State::kPaused, effective_utc_us);
    return true;
  }

  bool PlayoutControlStateMachine::Resume(const std::string &command_id,
                                          int64_t request_utc_us,
                                          int64_t effective_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true;
    }

    if (state_ != State::kPaused)
    {
      RecordIllegalTransitionLocked(state_, State::kPlaying);
      return false;
    }

    const double latency_ms =
        MicrosecondsToMilliseconds(effective_utc_us - request_utc_us);
    RecordLatencyLocked(resume_latencies_ms_, latency_ms);
    if (latency_ms > kResumeLatencyThresholdMs)
    {
      ++latency_violation_total_;
    }

    TransitionLocked(State::kPlaying, effective_utc_us);
    return true;
  }

  bool PlayoutControlStateMachine::Seek(const std::string &command_id,
                                        int64_t request_utc_us,
                                        int64_t target_pts_us,
                                        int64_t effective_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true;
    }

    if (target_pts_us < current_pts_us_)
    {
      ++late_seek_total_;
      return false;
    }

    if (state_ != State::kPlaying && state_ != State::kPaused)
    {
      RecordIllegalTransitionLocked(state_, State::kBuffering);
      return false;
    }

    const double latency_ms =
        MicrosecondsToMilliseconds(effective_utc_us - request_utc_us);
    RecordLatencyLocked(seek_latencies_ms_, latency_ms);
    if (latency_ms > kSeekLatencyThresholdMs)
    {
      ++latency_violation_total_;
    }

    TransitionLocked(State::kBuffering, request_utc_us);
    TransitionLocked(State::kReady, effective_utc_us);
    TransitionLocked(State::kPlaying, effective_utc_us);
    current_pts_us_ = target_pts_us;
    return true;
  }

  bool PlayoutControlStateMachine::Stop(const std::string &command_id,
                                        int64_t request_utc_us,
                                        int64_t effective_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true;
    }

    if (state_ == State::kIdle)
    {
      RecordIllegalTransitionLocked(state_, State::kStopping);
      return false;
    }

    const double latency_ms =
        MicrosecondsToMilliseconds(effective_utc_us - request_utc_us);
    RecordLatencyLocked(stop_latencies_ms_, latency_ms);
    if (latency_ms > kStopLatencyThresholdMs)
    {
      ++latency_violation_total_;
    }

    TransitionLocked(State::kStopping, request_utc_us);
    TransitionLocked(State::kIdle, effective_utc_us);
    return true;
  }

  bool PlayoutControlStateMachine::Recover(const std::string &command_id,
                                           int64_t request_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!RegisterCommandLocked(command_id))
    {
      return true;
    }

    if (state_ != State::kError)
    {
      RecordIllegalTransitionLocked(state_, State::kBuffering);
      return false;
    }

    TransitionLocked(State::kBuffering, request_utc_us);
    ++recover_total_;
    return true;
  }

  void PlayoutControlStateMachine::OnBufferDepth(std::size_t depth,
                                                 std::size_t capacity,
                                                 int64_t event_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (capacity == 0)
    {
      return;
    }

    if (state_ == State::kBuffering && depth >= kReadinessThresholdFrames)
    {
      TransitionLocked(State::kReady, event_utc_us);
      TransitionLocked(State::kPlaying, event_utc_us);
    }
    else if (state_ == State::kPlaying && depth == 0)
    {
      TransitionLocked(State::kBuffering, event_utc_us);
    }
  }

  void PlayoutControlStateMachine::OnBackPressureEvent(
      OrchestrationLoop::BackPressureEvent event,
      int64_t event_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event == OrchestrationLoop::BackPressureEvent::kUnderrun)
    {
      if (state_ == State::kPlaying)
      {
        TransitionLocked(State::kBuffering, event_utc_us);
      }
    }
    else if (event == OrchestrationLoop::BackPressureEvent::kOverrun)
    {
      // Currently treated as informational; no state change but recorded.
      ++queue_overflow_total_;
    }
  }

  void PlayoutControlStateMachine::OnBackPressureCleared(int64_t event_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::kBuffering)
    {
      TransitionLocked(State::kReady, event_utc_us);
      TransitionLocked(State::kPlaying, event_utc_us);
    }
  }

  void PlayoutControlStateMachine::OnExternalTimeout(int64_t event_utc_us)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++timeout_total_;
    TransitionLocked(State::kError, event_utc_us);
  }

  void PlayoutControlStateMachine::OnQueueOverflow()
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++queue_overflow_total_;
  }

  PlayoutControlStateMachine::State PlayoutControlStateMachine::state() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
  }

  PlayoutControlStateMachine::MetricsSnapshot PlayoutControlStateMachine::Snapshot()
      const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    MetricsSnapshot snapshot;
    snapshot.transitions = transitions_;
    snapshot.illegal_transition_total = illegal_transition_total_;
    snapshot.latency_violation_total = latency_violation_total_;
    snapshot.timeout_total = timeout_total_;
    snapshot.queue_overflow_total = queue_overflow_total_;
    snapshot.recover_total = recover_total_;
    snapshot.consistency_failure_total = consistency_failure_total_;
    snapshot.late_seek_total = late_seek_total_;
    snapshot.pause_latency_p95_ms = PercentileLocked(pause_latencies_ms_, 0.95);
    snapshot.resume_latency_p95_ms = PercentileLocked(resume_latencies_ms_, 0.95);
    snapshot.seek_latency_p95_ms = PercentileLocked(seek_latencies_ms_, 0.95);
    snapshot.stop_latency_p95_ms = PercentileLocked(stop_latencies_ms_, 0.95);
    snapshot.pause_deviation_p95_ms = PercentileLocked(pause_deviation_ms_, 0.95);
    if (!pause_latencies_ms_.empty())
    {
      snapshot.last_pause_latency_ms = pause_latencies_ms_.back();
    }
    if (!resume_latencies_ms_.empty())
    {
      snapshot.last_resume_latency_ms = resume_latencies_ms_.back();
    }
    if (!seek_latencies_ms_.empty())
    {
      snapshot.last_seek_latency_ms = seek_latencies_ms_.back();
    }
    if (!stop_latencies_ms_.empty())
    {
      snapshot.last_stop_latency_ms = stop_latencies_ms_.back();
    }
    if (!pause_deviation_ms_.empty())
    {
      snapshot.last_pause_deviation_ms = pause_deviation_ms_.back();
    }
    snapshot.state = state_;
    return snapshot;
  }

  void PlayoutControlStateMachine::TransitionLocked(State to, int64_t event_utc_us)
  {
    if (state_ == to)
    {
      return;
    }
    RecordTransitionLocked(state_, to);
    state_ = to;
    (void)event_utc_us;
  }

  void PlayoutControlStateMachine::RecordTransitionLocked(State from, State to)
  {
    transitions_[{from, to}]++;
  }

  void PlayoutControlStateMachine::RecordLatencyLocked(std::vector<double> &samples,
                                                       double value_ms)
  {
    samples.push_back(value_ms);
  }

  double PlayoutControlStateMachine::PercentileLocked(
      const std::vector<double> &samples,
      double percentile) const
  {
    if (samples.empty())
    {
      return 0.0;
    }

    std::vector<double> copy = samples;
    const double rank = percentile * static_cast<double>(copy.size() - 1);
    const std::size_t lower_index = static_cast<std::size_t>(std::floor(rank));
    const std::size_t upper_index = static_cast<std::size_t>(std::ceil(rank));
    std::nth_element(copy.begin(), copy.begin() + lower_index, copy.end());
    const double lower = copy[lower_index];
    if (upper_index == lower_index)
    {
      return lower;
    }
    std::nth_element(copy.begin(), copy.begin() + upper_index, copy.end());
    const double upper = copy[upper_index];
    const double fraction = rank - static_cast<double>(lower_index);
    return lower + (upper - lower) * fraction;
  }

  bool PlayoutControlStateMachine::RegisterCommandLocked(
      const std::string &command_id)
  {
    if (command_id.empty())
    {
      return true;
    }
    const auto [it, inserted] = processed_commands_.emplace(command_id, 1);
    if (!inserted)
    {
      return false;
    }
    return true;
  }

  void PlayoutControlStateMachine::RecordIllegalTransitionLocked(State from,
                                                                 State attempted_to)
  {
    ++illegal_transition_total_;
    transitions_[{from, attempted_to}]++;
  }

} // namespace retrovue::runtime
