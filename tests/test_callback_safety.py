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

linked = re.search(
    r"obs_audio_data \*linked_audio_clock_filter\(.*?\n\}",
    BRIDGE,
    flags=re.DOTALL,
)
if not linked:
    raise SystemExit("Could not isolate legacy receiver audio filter")
linked_callback = linked.group(0)
if "return audio;" not in linked_callback or "audio->frames" in linked_callback:
    raise SystemExit("Legacy receiver filter is not a strict pass-through")

route = re.search(
    r"\tbool route\(.*?\n\t\}(?=\n\n\tbool route_video)",
    BRIDGE,
    flags=re.DOTALL,
)
if not route:
    raise SystemExit("Could not isolate shared receiver audio correction path")
route_callback = route.group(0)
route_forbidden = tuple(token for token in forbidden_callback_tokens
                        if token not in ("std::unique_lock", "std::mutex"))
route_violations = [token for token in route_forbidden if token in route_callback]
if route_violations:
    raise SystemExit("Shared receiver correction contains forbidden operations: " + ", ".join(route_violations))
if "std::try_to_lock" not in route_callback:
    raise SystemExit("Receiver routing lock can block the real-time callback")
for marker in (
    "std::array<std::array<float, mcb::LinkedAudioPacketClock::kMaxOutputFrames>",
    "audio_packet_clock_.plan(audio->frames, audio->timestamp",
    "packet_plan.net_frame_adjustment",
    "obs_source_output_audio(outputs[static_cast<size_t>(pair)], &output)",
    "no filter changes a block",
):
    if marker not in BRIDGE:
        raise SystemExit(f"Shared receiver correction safety marker is missing: {marker}")

print("Callback safety audit passed")
