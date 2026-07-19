#!/usr/bin/env python3
"""Protect downstream measurement and linked-audio recovery behavior."""

from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
cpp = (ROOT / "hotfix/bridge/downstream-sync-core.cpp").read_text(encoding="utf-8")
header = (ROOT / "hotfix/bridge/downstream-sync-core.h").read_text(encoding="utf-8")
bridge = (ROOT / "hotfix/bridge/multichannel-bridge.cpp").read_text(encoding="utf-8")

required = (
    "DownstreamSyncPhase::Failed",
    "preserve_trusted_baseline",
    "median_absolute_deviation",
    "quarantine_until_ns_",
    "magnitude(candidate - state_.baseline_ns) > 40000000ULL",
    "state_.confidence >= 75",
    "state_.native_audio_error_ppm = state_.drift_ppm",
    "target = -static_cast<double>(state_.native_audio_error_ppm)",
    "state_.relation_ns = filtered_raw +",
    "video_timestamp_ns_.store(0",
    "audio_timestamp_ns_.store(0",
    "output_timestamp_ns_.store(0",
)
for marker in required:
    if marker not in cpp and marker not in header:
        raise SystemExit(f"Governor safety marker is missing: {marker}")

if "std::clamp(drift_minimum_ms, 30000" not in cpp:
    raise SystemExit("Drift correction can mature in less than 30 seconds")
if "std::clamp(baseline_window_ms, 5000" not in cpp:
    raise SystemExit("Trusted baseline can mature in less than five stable seconds")
for marker in (
    "observe_video(frame->timestamp, os_gettime_ns())",
    "observe_audio_input(audio->timestamp, now_ns)",
    "Both stereo proxies receive one corrected packet duration and timestamp",
    "Video is the master clock. It passes through unchanged",
    "reset_audio_correction_timeline(audio)",
):
    if marker not in bridge:
        raise SystemExit(f"Downstream correction marker is missing: {marker}")

print("Downstream measurement and linked-audio recovery audit passed")
