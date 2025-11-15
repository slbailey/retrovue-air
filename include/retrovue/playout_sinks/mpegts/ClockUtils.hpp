// Repository: Retrovue-playout
// Component: Clock Utilities
// Purpose: Thin wrappers for master clock operations and time conversions.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PLAYOUT_SINKS_MPEGTS_CLOCK_UTILS_HPP_
#define RETROVUE_PLAYOUT_SINKS_MPEGTS_CLOCK_UTILS_HPP_

#include <cstdint>
#include <memory>

namespace retrovue::timing {
class MasterClock;
}  // namespace retrovue::timing

namespace retrovue::playout_sinks::mpegts {

// ClockUtils provides thin wrappers for master clock operations.
// All timing operations should go through MasterClock, never direct system calls.
class ClockUtils {
 public:
  // Returns current UTC time in microseconds from master clock.
  // This is the primary time source - all timing decisions use this.
  static int64_t NowUtcUs(const std::shared_ptr<retrovue::timing::MasterClock>& master_clock);

  // TODO: Add conversion helpers for PTS to UTC mapping
  // TODO: Add helpers for computing sleep durations
  // TODO: Add helpers for frame gap calculations
};

}  // namespace retrovue::playout_sinks::mpegts

#endif  // RETROVUE_PLAYOUT_SINKS_MPEGTS_CLOCK_UTILS_HPP_

