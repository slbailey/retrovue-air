// Repository: Retrovue-playout
// Component: Clock Utilities
// Purpose: Thin wrappers for master clock operations and time conversions.
// Copyright (c) 2025 RetroVue

#include "retrovue/playout_sinks/mpegts/ClockUtils.hpp"
#include "retrovue/timing/MasterClock.h"

namespace retrovue::playout_sinks::mpegts {

int64_t ClockUtils::NowUtcUs(
    const std::shared_ptr<retrovue::timing::MasterClock>& master_clock) {
  if (!master_clock) {
    return 0;
  }
  return master_clock->now_utc_us();
}

// TODO: Add conversion helpers for PTS to UTC mapping
// TODO: Add helpers for computing sleep durations
// TODO: Add helpers for frame gap calculations

}  // namespace retrovue::playout_sinks::mpegts

