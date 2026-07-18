#!/usr/bin/env python3
"""Verify that every saved bridge setting has a default, read path, and write path."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "hotfix/bridge/multichannel-bridge.cpp").read_text(encoding="utf-8")

defaults = set(re.findall(r'config_set_default_(?:string|int|bool)\(config, kSection, "([^"]+)"', SOURCE))
writes = set(re.findall(r'config_set_(?!default_)(?:string|int|bool)\(config, kSection, "([^"]+)"', SOURCE))

missing_writes = sorted(defaults - writes)
if missing_writes:
    raise SystemExit("Settings with defaults but no save path: " + ", ".join(missing_writes))

missing_reads = []
for key in sorted(defaults):
    direct = re.search(rf'config_get_(?:string|int|bool)\(config, kSection, "{re.escape(key)}"', SOURCE)
    helper = re.search(rf'(?:config_string|config_track)\("{re.escape(key)}"', SOURCE)
    if not direct and not helper:
        missing_reads.append(key)
if missing_reads:
    raise SystemExit("Settings with defaults but no load/runtime path: " + ", ".join(missing_reads))

expected = {
    "Role", "TrackA", "TrackB", "ReceiverSource", "ProgramProxyName", "MicProxyName",
    "SuppressOriginal", "GovernorEnabled", "GovernorAutoConfigure", "GovernorMaxSkewMs",
    "GovernorVideoStallMs", "GovernorPlayoutDelayMs", "GovernorDriftCorrection",
    "GovernorMaxVideoCorrectionMs", "GovernorCorrectionSlewPpm", "GovernorRelockPairs",
    "GovernorBaselineWindowMs", "GovernorDriftWindowMs", "GovernorDriftMinimumMs",
    "GovernorDriftDeadbandPpm",
}
if defaults != expected:
    missing = sorted(expected - defaults)
    unexpected = sorted(defaults - expected)
    raise SystemExit(f"Settings manifest mismatch; missing={missing}, unexpected={unexpected}")

required_runtime_paths = (
    "main_output_init();",
    "ReceiverRouter::instance().configure_governor(",
    "ReceiverRouter::instance().set_suppress_original(",
    "mcb_sender_track_a_zero_based()",
    "mcb_sender_track_b_zero_based()",
    "Track A and Track B must be different",
)
for marker in required_runtime_paths:
    if marker not in SOURCE:
        raise SystemExit(f"Required runtime path is missing: {marker}")

print(f"Parameter path audit passed for {len(defaults)} saved settings")
