// Repository: Retrovue-playout
// Component: Producer Interface
// Purpose: Minimal interface for producers required by the contract.
// Copyright (c) 2025 RetroVue

#ifndef RETROVUE_PRODUCERS_IPRODUCER_H_
#define RETROVUE_PRODUCERS_IPRODUCER_H_

namespace retrovue::producers
{

  // IProducer defines the minimal interface required by the contract.
  // All producers must implement this interface.
  class IProducer
  {
  public:
    virtual ~IProducer() = default;

    // Starts the producer.
    // Returns true if started successfully, false if already running or on error.
    virtual bool start() = 0;

    // Stops the producer.
    // Blocks until the producer thread exits.
    virtual void stop() = 0;

    // Returns true if the producer is currently running.
    virtual bool isRunning() const = 0;
  };

} // namespace retrovue::producers

#endif // RETROVUE_PRODUCERS_IPRODUCER_H_







