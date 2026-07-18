#!/usr/bin/env python3
"""Static release gate for the game-PC real-time callback implementation."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
PATCHER = (ROOT / "hotfix/scripts/patch_distroav.py").read_text(encoding="utf-8")
CORE_H = (ROOT / "hotfix/bridge/sender-sync-core.h").read_text(encoding="utf-8")
CORE_CPP = (ROOT / "hotfix/bridge/sender-sync-core.cpp").read_text(encoding="utf-8")
BRIDGE = (ROOT / "hotfix/bridge/multichannel-bridge.cpp").read_text(encoding="utf-8")

match = re.search(r"raw_audio2 = r'''(.*?)'''", PATCHER, flags=re.DOTALL)
if not match:
    raise SystemExit("Could not locate injected raw_audio2 implementation")
callback = match.group(1)

forbidden_callback_tokens = (
    "std::vector", "std::deque", "std::mutex", "std::lock_guard", "std::unique_lock",
    ".resize(", ".reserve(", ".push_back(", ".emplace_back(", " new ", " delete ",
    "QFile", "QString", "obs_log(",
)
violations = [token for token in forbidden_callback_tokens if token in callback]
if violations:
    raise SystemExit("Real-time callback contains forbidden operations: " + ", ".join(violations))

forbidden_core_tokens = ("std::vector", "std::deque", "std::mutex", " new ", " delete ")
violations = [token for token in forbidden_core_tokens if token in CORE_H or token in CORE_CPP]
if violations:
    raise SystemExit("Sender Sync Core contains dynamic/blocking primitives: " + ", ".join(violations))

required = (
    "std::atomic_flag callback_busy",
    "SenderSyncCore::kOutputChannels",
    "mcb_sender_reanchor_generation()",
    "mcb_sender_counter_reset_generation()",
    "mcb_ui_monitoring_enabled()",
    "while (state->core.pop_output(output))",
)
for marker in required:
    if marker not in callback and marker not in PATCHER:
        raise SystemExit(f"Callback safety marker is missing: {marker}")

if "fade_scratch_" in BRIDGE and "std::array<std::array<float" not in BRIDGE:
    raise SystemExit("Receiver recovery scratch storage is not fixed-size")
if "scratch.resize(" in BRIDGE:
    raise SystemExit("Receiver audio callback can allocate fade scratch storage")

print("Callback safety audit passed")
