#!/usr/bin/env python3
"""Protect receiver availability, reconnect, and split-audio activation paths."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
bridge = (ROOT / "hotfix/bridge/multichannel-bridge.cpp").read_text(encoding="utf-8")
patcher = (ROOT / "hotfix/scripts/patch_distroav.py").read_text(encoding="utf-8")

required_bridge = (
    "Fail open. Timing protection must never make a live NDI source",
    "return true;\n\t}\n\n\tmcb::AVGovernorSnapshot governor_snapshot",
    "obs_source_set_audio_active(source, true);",
    "obs_source_set_muted(proxy, false);",
    "obs_source_set_audio_mixers(proxy, 0x3fU);",
    "bool force_reconnect()",
    'proc_handler_call(\n\t\t\tobs_source_get_proc_handler(source), "mcb_force_reconnect"',
)
for marker in required_bridge:
    if marker not in bridge:
        raise SystemExit(f"Receiver safety marker is missing: {marker}")

required_patcher = (
    "mcb_force_reconnect_proc",
    "void mcb_force_reconnect(out bool accepted)",
    "NDIlib_send_timecode_synthesize",
    "raw sender OBS timestamps leaked into NDI timecodes",
)
for marker in required_patcher:
    if marker not in patcher:
        raise SystemExit(f"Receiver patch contract is missing: {marker}")

print("Receiver availability and split-audio audit passed")
