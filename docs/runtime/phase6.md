_Status: Draft • Scope: Runtime Evolution_

# Phase 6 Overview

Phase 6 extends the cadence architecture introduced in Phase 5. The `MasterClock` remains the authoritative source of station time, while the PaceController consumes its signals to drive pacing decisions. `ChannelManager`, the `FrameProducer`, and the `MetricsPublisher` execute on that cadence, receiving tick notifications and deadlines from the controller. When viewer count reaches zero the control plane issues a `RequestTeardown`, allowing the producer to drain gracefully while the PaceController continues ticking until deregistration is confirmed. Test harnesses employ a stepped clock to deterministically advance time, enabling repeatable validation of timing contracts before we wire real hardware clocks into the loop. Concrete code changes will follow in subsequent milestones.
