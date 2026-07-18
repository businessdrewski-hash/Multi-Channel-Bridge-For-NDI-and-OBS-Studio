#!/usr/bin/env python3
"""Protect trusted-baseline recovery and incident-recorder behavior."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
cpp = (ROOT / "hotfix/bridge/av-governor.cpp").read_text(encoding="utf-8")
header = (ROOT / "hotfix/bridge/av-governor.h").read_text(encoding="utf-8")

hold = re.search(
    r"void AVGovernor::enter_hold_locked\(.*?\n\}(?=\n\nvoid AVGovernor::enter_failed_locked)",
    cpp,
    flags=re.DOTALL,
)
if not hold:
    raise SystemExit("Could not isolate governor hold transition")
if "state_.baseline_valid = false" in hold.group(0) or "state_.baseline_skew_ns = 0" in hold.group(0):
    raise SystemExit("Recovery once again destroys the trusted baseline")

required = (
    "AVGovernorReason::BaselineMismatch",
    "state_.fail_safe_bypassed = true",
    "median_absolute_deviation",
    "quarantine_until_ns_",
    "recovery_match_limit_ns_",
    "kCriticalRecorderCapacity",
    "kTelemetryRecorderCapacity",
    "telemetry_interval_ns = 1000000000ULL",
    "critical_events_",
    "telemetry_events_",
    "drift_confidence >= 75",
)
for marker in required:
    if marker not in cpp and marker not in header:
        raise SystemExit(f"Governor safety marker is missing: {marker}")

if "std::clamp(drift_minimum_ms, 30000" not in cpp:
    raise SystemExit("Drift correction can mature in less than 30 seconds")
if "std::clamp(baseline_window_ms, 5000" not in cpp:
    raise SystemExit("Trusted baseline can mature in less than five stable seconds")

print("Trusted-baseline and protected-recorder audit passed")
