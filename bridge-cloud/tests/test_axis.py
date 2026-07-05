"""Tests for the Grafana-faithful y-axis (uPlot rangeNum port + tick placement).

The expected bounds/ticks are read off real Grafana renders of the cold-chain
dashboard (folder f8jmvq, uid swxt2mq): Grafana hugs the data — soft bounds bind
while the data sits inside them, otherwise it pads ~10% past the data — and it
does NOT round the whole axis out to the next big tick.
"""
from __future__ import annotations

from grafana_push.data import nice_ticks, uplot_range


def _approx(a: float, b: float, tol: float = 0.35) -> bool:
    return abs(a - b) <= tol


def test_uplot_range_garage_freezer_soft_binds_both_sides() -> None:
    # soft [-2, 12], data sits inside → axis is exactly the soft bounds.
    lo, hi = uplot_range(-1.7, 6.0, soft_min=-2, soft_max=12)
    assert lo == -2.0
    assert hi == 12.0


def test_uplot_range_indoor_freezer_data_pokes_above_soft() -> None:
    # soft [-10, 10]; data max 10.5 exceeds soft_max → top extends past the data
    # (~13, NOT out to the next tick at 20). Bottom stays at the soft min.
    lo, hi = uplot_range(-6.5, 10.5, soft_min=-10, soft_max=10)
    assert lo == -10.0
    assert 11.0 <= hi <= 14.0
    assert hi < 20.0                      # the whole point: it does not reach 20


def test_uplot_range_indoor_fridge_soft_min_binds_data_max_extends() -> None:
    # soft [31, 40]; data max ~46.5 > 40 → top ~47.5. Bottom stays at soft min.
    lo, hi = uplot_range(37.0, 46.5, soft_min=31, soft_max=40)
    assert lo == 31.0
    assert 46.5 <= hi <= 48.0
    assert hi < 50.0                      # not rounded out to 50


def test_uplot_range_garage_fridge_soft_max_binds() -> None:
    # soft [31, 40]; data 35.5..38 inside → axis is the soft bounds.
    lo, hi = uplot_range(35.5, 38.0, soft_min=31, soft_max=40)
    assert lo == 31.0
    assert hi == 40.0


def test_uplot_range_no_soft_pads_data() -> None:
    # With no soft bounds it pads ~10% past the data and snaps to a nice incr.
    lo, hi = uplot_range(0.0, 60.0)
    assert lo <= 0.0
    assert hi >= 60.0
    assert hi < 80.0


def test_nice_ticks_fridge_uses_2_5_step_within_bounds() -> None:
    # 31..47.5 → Grafana shows 32.5, 35, ..., 47.5 (step 2.5); ticks stay inside.
    ticks = nice_ticks(31.0, 47.5)
    assert len(ticks) <= 8
    assert ticks[0] >= 31.0 and ticks[-1] <= 47.5 + 1e-6
    assert _approx(ticks[1] - ticks[0], 2.5, 0.01)
    assert 32.5 in ticks and 47.5 in ticks


def test_nice_ticks_freezer_step_5() -> None:
    ticks = nice_ticks(-10.0, 13.0)
    assert _approx(ticks[1] - ticks[0], 5.0, 0.01)
    assert ticks[0] == -10.0
    assert ticks[-1] == 10.0              # 15 would exceed the 13 top edge
    assert all(t <= 13.0 for t in ticks)


def test_nice_ticks_garage_freezer_step_2() -> None:
    ticks = nice_ticks(-2.0, 12.0)
    assert _approx(ticks[1] - ticks[0], 2.0, 0.01)
    assert ticks[0] == -2.0 and ticks[-1] == 12.0
    assert len(ticks) <= 8


def test_nice_ticks_thins_when_too_many() -> None:
    # A wide range must not blow the device's 8-label budget.
    ticks = nice_ticks(0.0, 1000.0, max_ticks=8)
    assert len(ticks) <= 8
