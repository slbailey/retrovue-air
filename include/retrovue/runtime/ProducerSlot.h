// Repository: Retrovue-playout
// Component: Producer Slot
// Purpose: Abstraction for managing a producer in a slot (preview or live).
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_RUNTIME_PRODUCER_SLOT_H_
#define RETROVUE_RUNTIME_PRODUCER_SLOT_H_

#include <memory>
#include <string>

namespace retrovue::producers {
  class IProducer;
}

namespace retrovue {

// ProducerSlot manages a single producer slot (preview or live).
// It tracks the producer, asset metadata, and loading state.
struct ProducerSlot {
  std::unique_ptr<producers::IProducer> producer;
  bool loaded = false;
  std::string asset_id;
  std::string file_path;

  // Resets the slot to empty state.
  void reset();
};

} // namespace retrovue

#endif // RETROVUE_RUNTIME_PRODUCER_SLOT_H_


