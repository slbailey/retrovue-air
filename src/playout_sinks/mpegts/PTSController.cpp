// Repository: Retrovue-playout
// Component: PTS Controller
// Purpose: Generates monotonically increasing PTS and maintains 90kHz clock mapping.
// Copyright (c) 2025 RetroVue

#include "retrovue/playout_sinks/mpegts/PTSController.hpp"

namespace retrovue::playout_sinks::mpegts {

PTSController::PTSController()
    : pts_base_90k_(0),
      first_frame_wallclock_us_(0),
      last_pts_90k_(0),
      initialized_(false) {}

PTSController::~PTSController() = default;

void PTSController::reset(int64_t first_frame_wallclock_us) {
  first_frame_wallclock_us_ = first_frame_wallclock_us;
  pts_base_90k_ = 0;  // Start PTS at 0 in 90kHz units
  last_pts_90k_ = 0;
  initialized_ = true;
}

bool PTSController::IsInitialized() const {
  return initialized_;
}

int64_t PTSController::ptsForFrameWallclock(int64_t frame_wallclock_us) {
  if (!initialized_) {
    return 0;  // Not initialized yet
  }

  // Calculate delta from first frame in microseconds
  int64_t delta_us = frame_wallclock_us - first_frame_wallclock_us_;

  // Convert to 90kHz: pts_90k = pts_base_90k_ + (delta_us * 90 / 1000)
  // Formula: delta_us * 90 / 1000 = delta_us * 0.09
  // To avoid floating point, use: (delta_us * 90) / 1000
  int64_t pts_90k = pts_base_90k_ + (delta_us * 90) / 1000;

  // Ensure monotonicity: if delta goes backwards, keep PTS increasing
  if (pts_90k <= last_pts_90k_) {
    // Frame timestamp went backwards, but PTS must be monotonic
    // Increment by minimum step (1 tick in 90kHz = ~11.1 microseconds)
    pts_90k = last_pts_90k_ + 1;
  }

  last_pts_90k_ = pts_90k;
  return pts_90k;
}

int64_t PTSController::GetFirstFrameWallclockUs() const {
  return first_frame_wallclock_us_;
}

}  // namespace retrovue::playout_sinks::mpegts

