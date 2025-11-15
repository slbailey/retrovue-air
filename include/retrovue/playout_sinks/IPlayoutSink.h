// Repository: Retrovue-playout
// Component: IPlayoutSink Interface
// Purpose: Base interface for all playout sinks.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_IPLAYOUT_SINK_H_
#define RETROVUE_PLAYOUT_SINKS_IPLAYOUT_SINK_H_

namespace retrovue::playout_sinks {

// IPlayoutSink is the base interface for all playout sinks.
// All sinks must own their timing loop and query MasterClock independently.
// MasterClock never pushes ticks or callbacks - sinks pull time via now_utc_us().
class IPlayoutSink {
 public:
  virtual ~IPlayoutSink() = default;

  // Starts the sink (opens socket, initializes encoder/muxer, starts worker thread).
  // Returns true if started successfully, false if already running.
  virtual bool start() = 0;

  // Stops the sink gracefully (stops worker thread, closes muxer, encoder, socket).
  virtual void stop() = 0;

  // Returns true if sink is currently running.
  virtual bool isRunning() const = 0;
};

}  // namespace retrovue::playout_sinks

#endif  // RETROVUE_PLAYOUT_SINKS_IPLAYOUT_SINK_H_





