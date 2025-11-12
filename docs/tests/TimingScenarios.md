_Related: [Playout runtime](../runtime/PlayoutRuntime.md) • [Contract testing scaffolding](ContractTesting.md) • [MasterClock domain contract](../contracts/MasterClockDomainContract.md)_

# Timing scenarios

## Purpose

Document the fast integration cadence test and long-haul timing soak so operators can run them consistently and understand how to read the resulting metrics against our accepted thresholds.

## Integration validation

- Builds the dedicated `integration_frame_cadence_tests` target and runs the registered `integration_frame_cadence` ctest entry.
- Asserts renderer pacing against the TestMasterClock for ten minutes of synthetic 29.97 fps content.

```powershell
cmake --build build --target integration_frame_cadence_tests
ctest --test-dir build --tests-regex integration_frame_cadence --output-on-failure
```

- Passing criteria:
  - Mean absolute gap stays below `10 ms`.
  - `p95` absolute gap stays below `4 ms`.
  - Total corrections remain at or below `600`.

## Soak validation

- Exercises the production gRPC control surface and renderer pipeline for an extended interval under optional CPU load.
- Emits aggregate timing diagnostics once per second and aborts when thresholds are exceeded.

```powershell
cmake --build build --target timing_soak
.\build\tools\soak\timing_soak.exe --channel-id 9001 --asset timing://soak/default --duration-seconds 900 --metrics-port 9300
```

- Optional flags:
  - `--cpu-load <0-100>` throttles a busy-loop stressor to validate behaviour under host contention.
  - `--duration-seconds` increases soak length (default `300` seconds).

- Automatic failure thresholds (enforced inside `timing_soak`):
  - `p95` absolute frame gap must remain ≤ `6 ms`.
  - Recent correction rate must remain ≤ `1.0` per second.

## Metrics interpretation

- `retrovue_playout_frame_gap_seconds{channel="…"}` mirrors the absolute gap tracked in both integration and soak scenarios. Multiply by `1,000` to compare against the `4 ms`/`6 ms` thresholds.
- `masterclock_corrections_total` and the per-second derivative surfaced in soak logs validate rule `MT_003` pace-controller convergence. Investigate spikes first in the MasterClock logs before tuning renderer pacing.
- `masterclock_jitter_ms_p95` stays correlated with frame gap. Sustained values above `5 ms` signal upstream clock noise; rerun the integration test with fresh TestMasterClock seeds to confirm reproducibility.
- Prometheus scrapes combine the MasterClock and Renderer samples illustrated in `docs/runtime/PlayoutRuntime.md`. Confirm scrape freshness before drawing conclusions from any single snapshot.


