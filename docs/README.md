# RetroVue Playout Engine — Documentation Index

The RetroVue Playout Engine is a native C++ subsystem responsible for frame-accurate video decoding and rendering. It interfaces with the Python ChannelManager via gRPC to provide continuous, clock-aligned frame streams for 24/7 broadcast operations.

This directory is organized by audience and purpose.

---

## Architecture

High-level system design and component relationships.

- [Architecture Overview](architecture/ArchitectureOverview.md) - Complete system architecture
- [Project Overview](PROJECT_OVERVIEW.md) - High-level project goals and phases

---

## Domain Models

Core concepts and entities that define what the playout engine is and what it does. These documents describe the "what" and "why," not the "how."

- [Playout Engine Domain](domain/PlayoutEngineDomain.md) - Core playout engine domain model
- [Renderer Domain](domain/RendererDomain.md) - Frame renderer subsystem
- [Metrics and Timing Domain](domain/MetricsAndTimingDomain.md) - Timing synchronization and telemetry

---

## Contracts

How we deliver and test the system. These define behavioral guarantees, API specifications, and testing requirements.

- [Playout Engine Contract](contracts/PlayoutEngineContract.md) - gRPC API specification and guarantees
- [Renderer Contract](contracts/RendererContract.md) - Renderer subsystem testing contract

---

## Runtime

How the playout engine behaves during operation.

- [Playout Runtime](runtime/PlayoutRuntime.md) - Runtime behavior and lifecycle

---

## Developer

Guides for developers working on or extending the playout engine.

- [Quick Start](developer/QuickStart.md) - Getting started guide
- [Build & Debug](developer/BuildAndDebug.md) - Build system and debugging workflow
- [Development Standards](developer/DevelopmentStandards.md) - Code structure and standards

---

## Infrastructure

Integration points and system boundaries.

- [Integration](infra/Integration.md) - Integration with RetroVue core system

---

## Testing

Testing strategy and specifications.

- [Playout Test Contract](testing/PlayoutTestContract.md) - Test requirements and validation

---

## Milestones

Development roadmap and historical milestones.

- [Roadmap](milestones/Roadmap.md) - Current and planned development phases
- [Phase 1 Complete](milestones/Phase1_Complete.md) - gRPC skeleton implementation
- [Phase 2 Plan](milestones/Phase2_Plan.md) - Phase 2 planning and objectives
- [Phase 2 Complete](milestones/Phase2_Complete.md) - Frame buffer and stub decode
- [Phase 3 Plan](milestones/Phase3_Plan.md) - Real decode and renderer plan
- [Phase 3 Complete](milestones/Phase3_Complete.md) - FFmpeg decoder, renderer, and HTTP metrics

---

## Related Projects

- [Retrovue Core](../../Retrovue/) - Python media asset manager and scheduling system
- [Proto Definitions](../proto/retrovue/playout.proto) - gRPC service definitions

---

## Documentation Principles

### Organization by Audience

- **Operators** → Runtime behavior, configuration
- **Developers** → Architecture, extension points, testing
- **Architects** → Domain models, contracts, integration

### Domain vs Contracts

- **Domain** = What the system is (entities, relationships, behavior)
- **Contracts** = How we test and deliver it (API specs, guarantees, validation)

### Standards

Follow the [Development Standards](development-standards.md) for code structure, naming conventions, and API design.

---

## Quick Links

### I want to...

- **Understand the system architecture** → [Architecture Overview](architecture/ArchitectureOverview.md)
- **Build and run the playout engine** → [Quick Start](developer/QuickStart.md)
- **Understand the gRPC API** → [Playout Contract](contracts/PlayoutContract.md)
- **See the development roadmap** → [Roadmap](milestones/Roadmap.md)
- **Contribute code** → [Development Standards](developer/DevelopmentStandards.md)
- **Debug a build issue** → [Build & Debug](developer/BuildAndDebug.md)

---

## Status

**Current Phase:** Phase 3 ✅ Complete  
**Latest Milestone:** Renderer and HTTP metrics implementation  
**Next Phase:** Phase 4 - Production hardening and multi-channel support

---

**For questions or issues, see the main [README](../README.md) or consult the [Project Overview](PROJECT_OVERVIEW.md).**
