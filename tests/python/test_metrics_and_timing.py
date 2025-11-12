# Copyright (c) 2025 RetroVue
#
# RetroVue Air behavioral integration tests (Python harness)
#
# These tests mirror the legacy RetroVue Core pacing scenarios, but execute against
# simple fakes so they remain deterministic inside the Air repo. Once the shared
# TestMasterClock / PaceController harness lands in retrovue-core these fakes can be
# swapped out for the real implementations with minimal changes.

from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

import pytest


@dataclass
class MetricsSample:
    """Captured snapshot emitted by the pacing loop."""

    utc_us: int
    payload: Dict[str, float]
    freshness_deadline_us: int


class FakeTestMasterClock:
    """Deterministic stepped clock used only for tests."""

    def __init__(self, epoch_us: int = 1_700_000_000_000_000) -> None:
        self._utc_us = epoch_us

    @property
    def utc_us(self) -> int:
        return self._utc_us

    def advance(self, delta_us: int) -> int:
        if delta_us < 0:
            raise ValueError("Test clock cannot move backwards")
        self._utc_us += delta_us
        return self._utc_us


class FakeMetricsPublisher:
    """In-memory metrics sink that enforces freshness semantics."""

    def __init__(self, freshness_budget_us: int = 100_000) -> None:
        self._freshness_budget_us = freshness_budget_us
        self._samples: List[MetricsSample] = []

    def publish(self, utc_us: int, payload: Dict[str, float]) -> None:
        deadline = utc_us + self._freshness_budget_us
        self._samples.append(MetricsSample(utc_us, payload, deadline))
        cutoff = utc_us - self._freshness_budget_us
        self._samples = [sample for sample in self._samples if sample.utc_us >= cutoff]

    def last_sample(self) -> Optional[MetricsSample]:
        return self._samples[-1] if self._samples else None

    def samples(self) -> List[MetricsSample]:
        return list(self._samples)


class FakeProgramDirector:
    """Tracks channel lifecycle and produces per-tick telemetry payloads."""

    def __init__(self) -> None:
        self._state = "READY"
        self._total_frames = 0
        self._teardown_requested = False
        self._teardown_completed = False
        self._teardown_deadline_us: Optional[int] = None

    @property
    def state(self) -> str:
        return self._state

    @property
    def teardown_completed(self) -> bool:
        return self._teardown_completed

    def on_tick(self, utc_us: int) -> Dict[str, float]:
        if self._teardown_requested and not self._teardown_completed:
            # Keep emitting until deadline, then mark stopped.
            if self._teardown_deadline_us is not None and utc_us >= self._teardown_deadline_us:
                self._state = "STOPPED"
                self._teardown_completed = True

        if self._state != "STOPPED":
            self._total_frames += 1

        return {
            "frames_total": float(self._total_frames),
            "state_ready": 1.0 if self._state == "READY" else 0.0,
            "state_stopped": 1.0 if self._state == "STOPPED" else 0.0,
        }

    def request_teardown(self, utc_us: int, drain_us: int) -> None:
        self._teardown_requested = True
        self._teardown_deadline_us = utc_us + drain_us


class FakePaceController:
    """Single-threaded tick loop that advances the test clock and records metrics."""

    def __init__(
        self,
        clock: FakeTestMasterClock,
        publisher: FakeMetricsPublisher,
        director: FakeProgramDirector,
        tick_period_us: int,
    ) -> None:
        if tick_period_us <= 0:
            raise ValueError("tick_period_us must be positive")
        self._clock = clock
        self._publisher = publisher
        self._director = director
        self._tick_period_us = tick_period_us
        self._tick_counter = 0

    def run_step_once_for_tests(self) -> None:
        """Advance the clock, obtain telemetry, and publish it."""
        self._clock.advance(self._tick_period_us)
        payload = self._director.on_tick(self._clock.utc_us)
        self._publisher.publish(self._clock.utc_us, payload)
        self._tick_counter += 1

    @property
    def tick_counter(self) -> int:
        return self._tick_counter

    @property
    def tick_period_us(self) -> int:
        return self._tick_period_us

    @property
    def clock(self) -> FakeTestMasterClock:
        return self._clock

    @property
    def publisher(self) -> FakeMetricsPublisher:
        return self._publisher

    @property
    def director(self) -> FakeProgramDirector:
        return self._director


# --------------------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------------------


def _make_harness(tick_period_us: int = 33_366) -> FakePaceController:
    clock = FakeTestMasterClock()
    publisher = FakeMetricsPublisher(freshness_budget_us=3 * tick_period_us)
    director = FakeProgramDirector()
    return FakePaceController(clock, publisher, director, tick_period_us)


def test_deterministic_tick_cadence():
    controller = _make_harness()
    publisher = controller.publisher
    clock = controller.clock

    expected_interval = controller.tick_period_us

    controller.run_step_once_for_tests()
    first_sample = publisher.last_sample()
    assert first_sample is not None

    controller.run_step_once_for_tests()
    second_sample = publisher.last_sample()
    assert second_sample is not None
    assert second_sample.utc_us - first_sample.utc_us == expected_interval

    for _ in range(8):
        controller.run_step_once_for_tests()

    timeline = publisher.samples()
    intervals = [
        timeline[i].utc_us - timeline[i - 1].utc_us for i in range(1, len(timeline))
    ]
    assert all(interval == expected_interval for interval in intervals), intervals
    assert len(timeline) >= 3
    assert controller.tick_counter >= len(timeline)
    assert clock.utc_us == timeline[-1].utc_us


def test_metrics_samples_remain_fresh():
    controller = _make_harness()
    publisher = controller.publisher

    for _ in range(5):
        controller.run_step_once_for_tests()

    samples = publisher.samples()
    assert samples, "Expected non-empty telemetry stream"

    budget_us = publisher.last_sample().freshness_deadline_us - publisher.last_sample().utc_us  # type: ignore[union-attr]
    for sample in samples:
        age_us = samples[-1].utc_us - sample.utc_us
        assert age_us <= budget_us, f"Sample aged {age_us} us which exceeds freshness budget {budget_us}"


def test_teardown_completes_with_final_snapshot():
    controller = _make_harness()
    publisher = controller.publisher
    director = controller.director
    clock = controller.clock

    for _ in range(3):
        controller.run_step_once_for_tests()

    director.request_teardown(clock.utc_us, drain_us=2 * controller.tick_period_us)

    # Continue stepping until teardown completes.
    while not director.teardown_completed:
        controller.run_step_once_for_tests()

    last_sample = publisher.last_sample()
    assert last_sample is not None
    assert last_sample.payload["state_stopped"] == pytest.approx(1.0)
    assert director.state == "STOPPED"
    # Ensure a final sample was published at or after the deadline.
    assert last_sample.utc_us >= clock.utc_us

