"""Tests for the v3 "color bundle" (reTerminal E1004, E Ink Spectra 6): the
Grafana-colour → Spectra 6 mapping and the version-3 encode/decode path. The
wire structure is shared with the v2 gray bundle, so only color semantics and
the version stamp are covered here."""
from __future__ import annotations

from grafana_push.data_trmnl import (
    BUNDLE_VERSION_COLOR,
    PANEL_STAT,
    PANEL_TIMESERIES,
    SPECTRA_BLACK,
    SPECTRA_BLUE,
    SPECTRA_GREEN,
    SPECTRA_RED,
    SPECTRA_WHITE,
    SPECTRA_YELLOW,
    active_threshold_spectra,
    band_spectra_for_color,
    bands_from_steps,
    decode_dashboard_bundle,
    encode_dashboard_bundle,
    encode_dashboard_bundle_fit,
    spectra_for_color,
)


def test_spectra_for_color_names() -> None:
    assert spectra_for_color("red") == SPECTRA_RED
    assert spectra_for_color("semi-dark-red") == SPECTRA_RED
    assert spectra_for_color("yellow") == SPECTRA_YELLOW
    assert spectra_for_color("#EAB839") == SPECTRA_YELLOW     # Grafana gold
    assert spectra_for_color("orange") == SPECTRA_YELLOW      # no orange on Spectra 6
    assert spectra_for_color("green") == SPECTRA_GREEN
    assert spectra_for_color("dark-green") == SPECTRA_GREEN
    assert spectra_for_color("blue") == SPECTRA_BLUE
    assert spectra_for_color("purple") == SPECTRA_BLUE
    assert spectra_for_color("text") == SPECTRA_BLACK
    assert spectra_for_color(None) == SPECTRA_WHITE
    assert spectra_for_color("") == SPECTRA_WHITE


def test_spectra_for_color_hex_and_rgba_hue_wins() -> None:
    # Hue, not luminance: a dim red is still red on a color panel.
    assert spectra_for_color("rgba(255, 0, 0, 0.1)") == SPECTRA_RED
    assert spectra_for_color("#c4162a") == SPECTRA_RED        # Grafana dark-red
    assert spectra_for_color("rgb(0, 200, 0)") == SPECTRA_GREEN
    assert spectra_for_color("#1f60c4") == SPECTRA_BLUE       # Grafana semi-dark-blue
    assert spectra_for_color("#fade2a") == SPECTRA_YELLOW     # Grafana yellow
    assert spectra_for_color("#ffffff") == SPECTRA_WHITE
    assert spectra_for_color("#000000") == SPECTRA_BLACK


def test_band_spectra_green_draws() -> None:
    # Unlike the gray bundle, green zones DO render on the color panel (as a
    # light green dither) — Grafana's area style paints every zone.
    assert band_spectra_for_color("green") == SPECTRA_GREEN
    assert band_spectra_for_color("rgba(0, 200, 0, 0.1)") == SPECTRA_GREEN
    assert band_spectra_for_color("red") == SPECTRA_RED
    assert band_spectra_for_color("yellow") == SPECTRA_YELLOW
    assert band_spectra_for_color("transparent") == SPECTRA_WHITE


def test_active_threshold_spectra_picks_band() -> None:
    steps = [(None, "red"), (0.0, "red"), (25.0, "yellow"), (30.0, "green"), (40.0, "blue")]
    assert active_threshold_spectra(steps, 18) == SPECTRA_RED
    assert active_threshold_spectra(steps, 28) == SPECTRA_YELLOW
    assert active_threshold_spectra(steps, 33) == SPECTRA_GREEN
    assert active_threshold_spectra(steps, 55) == SPECTRA_BLUE
    assert active_threshold_spectra(steps, None) == SPECTRA_WHITE


def test_bands_from_steps_spectra_cold_chain() -> None:
    # Same fridge shape as the gray test, but every zone carries its real hue:
    # red out-of-range zones AND the green in-range zone all render.
    steps = [(None, "red"), (0.0, "red"), (33.0, "green"), (38.0, "red")]
    bands = bands_from_steps(steps, 31.0, 40.0,
                             shade=band_spectra_for_color, none_value=SPECTRA_WHITE)
    assert len(bands) == 3
    assert bands[0][2] == SPECTRA_RED
    assert bands[1][2] == SPECTRA_GREEN           # in-range zone is a green band
    assert bands[2][2] == SPECTRA_RED
    assert abs(bands[-1][1] - 1.0) < 1e-6


def _color_dashboards() -> list[dict]:
    return [{
        "title": "1", "grid_cols": 24, "grid_rows": 9,
        "panels": [
            {
                "type": PANEL_TIMESERIES, "gx": 0, "gy": 0, "gw": 12, "gh": 9,
                "title": "Fridge (F)", "unit": "", "base_gray": SPECTRA_WHITE,
                "y_labels": ["31", "40"], "x_labels": [],
                "bands": [(0.0, 0.22, SPECTRA_RED), (0.78, 1.0, SPECTRA_YELLOW)],
                "series": [[(0.0, 0.5), (1.0, 0.55)]],
            },
            {
                "type": PANEL_STAT, "gx": 12, "gy": 0, "gw": 12, "gh": 9,
                "title": "Soil", "unit": "%", "base_gray": SPECTRA_GREEN,
                "value_str": "42", "sparkline": [(0.0, 0.4), (1.0, 0.5)],
            },
        ],
    }]


def test_color_bundle_roundtrip_version3() -> None:
    body, etag = encode_dashboard_bundle(
        _color_dashboards(), next_poll=600, version=BUNDLE_VERSION_COLOR)
    dec = decode_dashboard_bundle(body)
    assert dec["magic"] == 0xCFB2
    assert dec["version"] == BUNDLE_VERSION_COLOR
    p = dec["dashboards"][0]["panels"][0]
    assert p["bands"][0][2] == SPECTRA_RED
    assert p["bands"][1][2] == SPECTRA_YELLOW
    s = dec["dashboards"][0]["panels"][1]
    assert s["base_gray"] == SPECTRA_GREEN        # field name is shared; v3 = color code


def test_color_bundle_fit_keeps_version() -> None:
    d = _color_dashboards()
    body, _etag, info = encode_dashboard_bundle_fit(
        d, next_poll=600, budget=1 << 20, version=BUNDLE_VERSION_COLOR)
    assert decode_dashboard_bundle(body)["version"] == BUNDLE_VERSION_COLOR
    assert info["downsampled"] is False


def test_gray_bundle_unchanged_by_default() -> None:
    # The default encode path still stamps v2 — the TRMNL X is untouched.
    body, _ = encode_dashboard_bundle(_color_dashboards(), next_poll=600)
    assert decode_dashboard_bundle(body)["version"] == 2


def test_e1004_urls_do_not_touch_trmnl_objects() -> None:
    # Regression: push_e1004 must derive its own manifest/capacity paths —
    # importing push_trmnl's helpers silently overwrote /manifest-trmnl.
    from grafana_push import push_e1004
    base = "https://dashboard.example.net/bundle-e1004"
    assert push_e1004._manifest_url(base) == "https://dashboard.example.net/manifest-e1004"
