from __future__ import annotations

from grafana_push.data_trmnl import (
    BAND_GRAY_ALERT,
    BAND_GRAY_WARN,
    GRAY_WHITE,
    PANEL_STAT,
    PANEL_TIMESERIES,
    active_threshold_gray,
    band_gray_for_color,
    bands_from_steps,
    decode_dashboard_bundle,
    decode_manifest,
    encode_dashboard_bundle,
    encode_dashboard_bundle_fit,
    encode_manifest,
    etag32,
    gray_for_color,
)


def test_gray_for_color_names_and_hex() -> None:
    assert gray_for_color("red") == 5
    assert gray_for_color("green") == 12
    assert gray_for_color("yellow") == 9
    assert gray_for_color("#EAB839") == 9          # Grafana gold
    assert gray_for_color("semi-dark-red") == 5    # prefix stripped
    assert gray_for_color(None) == GRAY_WHITE
    assert gray_for_color("") == GRAY_WHITE
    # A pure-white hex maps to white, pure black to black.
    assert gray_for_color("#ffffff") == 15
    assert gray_for_color("#000000") == 0
    # rgb()/rgba() strings (as Grafana threshold colours emit) map by luminance.
    assert gray_for_color("rgba(255, 0, 0, 0.1)") == 4    # dim red
    assert gray_for_color("rgb(0, 200, 0)") == 7          # mid green
    assert gray_for_color("rgba(255,255,255,1)") == 15


def test_band_gray_for_color_red_vs_yellow() -> None:
    # In-range green draws no fill; red and yellow get distinct light shades.
    assert band_gray_for_color("rgba(0, 200, 0, 0.1)") == GRAY_WHITE
    assert band_gray_for_color("green") == GRAY_WHITE
    assert band_gray_for_color("rgba(255, 0, 0, 0.1)") == BAND_GRAY_ALERT
    assert band_gray_for_color("red") == BAND_GRAY_ALERT
    assert band_gray_for_color("rgba(255, 255, 0, 1)") == BAND_GRAY_WARN
    assert band_gray_for_color("yellow") == BAND_GRAY_WARN
    assert BAND_GRAY_ALERT != BAND_GRAY_WARN   # red and yellow are different


def test_active_threshold_gray_picks_band() -> None:
    # soil moisture: red@0, yellow@25, green@30, blue@40
    steps = [(None, "red"), (0.0, "red"), (25.0, "yellow"), (30.0, "green"), (40.0, "blue")]
    assert active_threshold_gray(steps, 18) == gray_for_color("red")
    assert active_threshold_gray(steps, 28) == gray_for_color("yellow")
    assert active_threshold_gray(steps, 33) == gray_for_color("green")
    assert active_threshold_gray(steps, 55) == gray_for_color("blue")
    assert active_threshold_gray(steps, None) == GRAY_WHITE


def test_bands_from_steps_cold_chain() -> None:
    # fridge: red below 33, green 33-38, red above; axis 31..40. The in-range
    # (green) middle draws NO band, leaving two red out-of-range bands.
    steps = [(None, "red"), (0.0, "red"), (33.0, "green"), (38.0, "red")]
    bands = bands_from_steps(steps, 31.0, 40.0)
    assert len(bands) == 2
    assert bands[0][0] == 0.0                       # low red band starts at axis min
    assert abs(bands[-1][1] - 1.0) < 1e-6           # high red band reaches the top
    assert all(g == BAND_GRAY_ALERT for _, _, g in bands)
    # the clean gap between the two red bands is the in-range zone
    assert bands[0][1] < bands[1][0]


def test_bands_from_steps_rgba_and_unsorted() -> None:
    # Real Grafana data: rgba colours, and steps stored out of value order.
    steps = [(0.0, "rgba(255,0,0,0.1)"), (-8.0, "rgba(0,200,0,0.1)"), (4.0, "rgba(255,0,0,0.1)")]
    bands = bands_from_steps(steps, -10.0, 10.0)
    # green (in-range) drops out; the two red zones remain, correctly ordered.
    assert bands
    assert all(g == BAND_GRAY_ALERT for _, _, g in bands)
    assert bands == sorted(bands)


def _sample_dashboards() -> list[dict]:
    return [
        {
            "title": "1",
            "grid_cols": 24,
            "grid_rows": 18,
            "panels": [
                {
                    "type": PANEL_TIMESERIES, "gx": 0, "gy": 0, "gw": 12, "gh": 9,
                    "title": "Indoor Fridge (F)", "unit": "", "base_gray": 15,
                    "y_labels": ["31", "34", "37", "40"],
                    "x_labels": ["0h", "12h", "now"],
                    "bands": [(0.0, 0.22, 5), (0.22, 0.78, 12), (0.78, 1.0, 5)],
                    "series": [[(0.0, 0.5), (0.5, 0.6), (1.0, 0.55)], [(0.0, 0.3), (1.0, 0.35)]],
                },
            ],
        },
        {
            "title": "2",
            "grid_cols": 24,
            "grid_rows": 7,
            "panels": [
                {
                    "type": PANEL_STAT, "gx": 0, "gy": 0, "gw": 12, "gh": 7,
                    "title": "Golden Pothos", "unit": "%", "base_gray": 9,
                    "value_str": "28", "sparkline": [(0.0, 0.4), (1.0, 0.5)],
                },
            ],
        },
    ]


def test_bundle_roundtrip() -> None:
    body, etag = encode_dashboard_bundle(_sample_dashboards(), next_poll=600)
    assert len(etag) == 16
    dec = decode_dashboard_bundle(body)
    assert dec["magic"] == 0xCFB2
    assert dec["version"] == 1
    assert dec["next_poll"] == 600
    assert len(dec["dashboards"]) == 2

    d0 = dec["dashboards"][0]
    assert d0["title"] == "1"
    assert d0["grid_rows"] == 18
    p = d0["panels"][0]
    assert p["type"] == PANEL_TIMESERIES
    assert p["title"] == "Indoor Fridge (F)"
    assert p["y_labels"] == ["31", "34", "37", "40"]
    assert len(p["bands"]) == 3
    assert len(p["series"]) == 2            # multi-series preserved
    assert len(p["series"][0]) == 3
    # normalized points survive the u16 quantization within ~1/65535
    assert abs(p["series"][0][1][0] - 0.5) < 1e-4

    s = dec["dashboards"][1]["panels"][0]
    assert s["type"] == PANEL_STAT
    assert s["value_str"] == "28"
    assert s["unit"] == "%"
    assert s["base_gray"] == 9
    assert len(s["sparkline"]) == 2


def test_bundle_etag_stable() -> None:
    d = _sample_dashboards()
    b1, e1 = encode_dashboard_bundle(d, next_poll=600)
    b2, e2 = encode_dashboard_bundle(d, next_poll=600)
    assert b1 == b2 and e1 == e2


def _big_dashboards(n_points: int) -> list[dict]:
    """One timeseries dashboard with a single dense series, for fit tests."""
    pts = [(i / (n_points - 1), (i % 7) / 7.0) for i in range(n_points)]
    return [{
        "title": "big", "grid_cols": 24, "grid_rows": 9,
        "panels": [{
            "type": PANEL_TIMESERIES, "gx": 0, "gy": 0, "gw": 24, "gh": 9,
            "title": "dense", "unit": "", "base_gray": 15,
            "y_labels": ["0", "1"], "x_labels": [], "bands": [],
            "series": [pts],
        }],
    }]


def test_fit_no_downsample_under_budget() -> None:
    # A generous budget leaves the bundle at full resolution.
    d = _big_dashboards(1000)
    body, etag, info = encode_dashboard_bundle_fit(d, next_poll=600, budget=1 << 20)
    assert info["downsampled"] is False
    assert info["kept_points"] == 1000
    # Identical to a plain full-res encode.
    plain, _ = encode_dashboard_bundle(d, next_poll=600)
    assert body == plain
    assert decode_dashboard_bundle(body)["dashboards"][0]["panels"][0]["series"][0]


def test_fit_downsamples_to_budget() -> None:
    # A tight budget forces the series to be decimated until the bundle fits.
    d = _big_dashboards(4000)
    budget = 8 * 1024
    body, etag, info = encode_dashboard_bundle_fit(d, next_poll=600, budget=budget)
    assert info["downsampled"] is True
    assert len(body) <= budget
    assert info["size"] == len(body)
    # Kept the most points that still fit — one more would overflow.
    kept = info["kept_points"]
    assert 2 <= kept < 4000
    dec = decode_dashboard_bundle(body)
    series = dec["dashboards"][0]["panels"][0]["series"][0]
    assert len(series) == kept
    # Endpoints are preserved by the decimation (first & last kept).
    assert abs(series[0][0] - 0.0) < 1e-4
    assert abs(series[-1][0] - 1.0) < 1e-4


def test_fit_zero_budget_is_full_res() -> None:
    # budget<=0 means "unknown" — never downsample on a missing/zero budget.
    d = _big_dashboards(500)
    body, _, info = encode_dashboard_bundle_fit(d, next_poll=600, budget=0)
    assert info["downsampled"] is False
    assert info["kept_points"] == 500


def test_manifest_roundtrip() -> None:
    # Two dashboards, distinct etags → distinct u32s the device can compare.
    etags = ["ab12cd34ef", "00000000ff"]
    body = encode_manifest(etags, next_poll=600)
    dec = decode_manifest(body)
    assert dec["magic"] == 0xCFB3
    assert dec["version"] == 1
    assert dec["count"] == 2
    assert dec["next_poll"] == 600
    assert dec["etags"] == [etag32("ab12cd34ef"), etag32("00000000ff")]
    assert dec["etags"][0] == 0xAB12CD34
    # A changed dashboard yields a different manifest entry.
    assert etag32("ab12cd34ef") != etag32("ab12cd35ef")


def test_manifest_matches_per_dashboard_encode() -> None:
    # The manifest etag for dashboard i must equal that dashboard's own
    # single-dashboard bundle etag, so the device's change detection lines up.
    dashboards = _sample_dashboards()
    per_etags = []
    for d in dashboards:
        _, etag, _ = encode_dashboard_bundle_fit([d], next_poll=600, budget=0)
        per_etags.append(etag)
    man = decode_manifest(encode_manifest(per_etags, 600))
    assert man["count"] == len(dashboards)
    assert man["etags"] == [etag32(e) for e in per_etags]
