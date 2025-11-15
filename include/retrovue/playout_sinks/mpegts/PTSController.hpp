// Repository: Retrovue-playout
// Component: PTS Controller
// Purpose: Generates monotonically increasing PTS and maintains 90kHz clock mapping.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_PTS_CONTROLLER_HPP_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_PTS_CONTROLLER_HPP_

#include <cstdint>

namespace retrovue::playout_sinks::mpegts {

// PTSController manages presentation timestamp generation and mapping.
// It maintains a monotonically increasing PTS sequence in 90kHz clock units
// and maps media PTS (microseconds) to station time using an affine transform.
class PTSController {
 public:
  PTSController();
  ~PTSController();

  // Disable copy and move
  PTSController(const PTSController&) = delete;
  PTSController& operator=(const PTSController&) = delete;
  PTSController(PTSController&&) = delete;
  PTSController& operator=(PTSController&&) = delete;

  // Reset PTS controller with first frame.
  // first_frame_wallclock_us: Wall clock time when first frame arrives (microseconds)
  // Sets up the base PTS and initializes the mapping.
  void reset(int64_t first_frame_wallclock_us);

  // Calculate PTS in 90kHz units for a frame given its wall clock time.
  // frame_wallclock_us: Wall clock time of the frame (microseconds)
  // Returns PTS in 90kHz clock units (int64_t)
  // Maintains monotonicity even if input timestamps go backwards.
  int64_t ptsForFrameWallclock(int64_t frame_wallclock_us);

  // Check if PTS controller has been initialized.
  bool IsInitialized() const;

  // Get the PTS zero point (wall clock time when first frame arrived).
  // Only valid after reset() has been called.
  int64_t GetFirstFrameWallclockUs() const;

 private:
  int64_t pts_base_90k_;              // Base PTS in 90kHz units
  int64_t first_frame_wallclock_us_;   // Wall clock time of first frame
  int64_t last_pts_90k_;               // Last emitted PTS (for monotonicity)
  bool initialized_;
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_PTS_CONTROLLER_HPP_

