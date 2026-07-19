#!/usr/bin/env python3
"""Summarize a Receiver Clock Lab CSV using only the Python standard library."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from pathlib import Path


def number(row: dict[str, str], key: str) -> float:
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return math.nan


def median(values: list[float]) -> float:
    finite = [value for value in values if math.isfinite(value)]
    return statistics.median(finite) if finite else math.nan


def robust_change_ms(rows: list[dict[str, str]], field: str, window: int) -> tuple[float, float, float]:
    first = median([number(row, field) for row in rows[:window]]) / 1e6
    last = median([number(row, field) for row in rows[-window:]]) / 1e6
    return first, last, last - first


def counter_change(rows: list[dict[str, str]], field: str) -> float:
    return number(rows[-1], field) - number(rows[0], field)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--window", type=int, default=120, help="rows in first/last robust windows")
    parser.add_argument("--warmup-seconds", type=float, default=10.0)
    args = parser.parse_args()

    with args.csv_path.open(newline="", encoding="utf-8-sig") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        raise SystemExit("CSV contains no samples")

    required = {
        "sample_wall_ns",
        "mode",
        "selected_video_minus_filtered_audio_projected_ns",
        "ndi_dropped_audio_frames",
        "ndi_dropped_video_frames",
    }
    missing = sorted(required - rows[0].keys())
    if missing:
        raise SystemExit(f"Not a Receiver Clock Lab CSV; missing fields: {', '.join(missing)}")

    first_wall = number(rows[0], "sample_wall_ns")
    cutoff = first_wall + args.warmup_seconds * 1e9
    warmed = [row for row in rows if number(row, "sample_wall_ns") >= cutoff]
    if len(warmed) >= 8:
        rows = warmed

    duration = (number(rows[-1], "sample_wall_ns") - number(rows[0], "sample_wall_ns")) / 1e9
    mode = rows[-1].get("mode", "unknown")
    mode_name = {"0": "stock-direct", "1": "stock-framesync", "2": "receiver-paced"}.get(mode, "unknown")
    print(f"rows={len(rows)} duration_s={duration:.3f} mode={mode} ({mode_name})")

    fields = [
        "capture_video_minus_capture_audio_projected_ns",
        "output_video_minus_output_audio_projected_ns",
        "selected_video_minus_output_video_projected_ns",
        "filtered_audio_minus_output_audio_projected_ns",
        "selected_video_minus_filtered_audio_projected_ns",
    ]
    window = min(args.window, max(1, len(rows) // 4))
    for field in fields:
        first, last, change = robust_change_ms(rows, field, window)
        slope_ppm = change * 1000.0 / duration if duration > 0 else math.nan
        print(f"{field}: {first:.6f} -> {last:.6f} ms; change={change:+.6f} ms; slope={slope_ppm:+.3f} ppm")

    print("counters:")
    for field in [
        "ndi_dropped_audio_frames",
        "ndi_dropped_video_frames",
        "audio_catchups",
        "video_catchups",
        "repeated_video_frames",
        "empty_audio_pulls",
        "empty_video_pulls",
    ]:
        print(f"  {field}={counter_change(rows, field):.0f}")
    for field in ["ndi_queued_audio_frames", "ndi_queued_video_frames"]:
        finite = [number(row, field) for row in rows if math.isfinite(number(row, field))]
        print(f"  max_{field}={max(finite) if finite else math.nan:.0f}")


if __name__ == "__main__":
    main()
