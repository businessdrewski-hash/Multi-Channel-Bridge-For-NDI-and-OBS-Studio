#!/usr/bin/env python3
"""Protect the compact, plain-language monitoring surface."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
bridge = (ROOT / "hotfix/bridge/multichannel-bridge.cpp").read_text(encoding="utf-8")

required = (
    'setup_toggle_->setText("▸ Setup")',
    'numbers_toggle_->setText("▸ Numbers")',
    "configuration_panel_->setVisible",
    "monitor_numbers_panel_->setVisible",
    "Sender protected",
    "Desktop + mic rushing",
    "Video rushing",
    "Suggested:</b>",
    "Needs attention",
    "Correcting slow drift",
    "QApplication::windowIcon()",
    "top->setWindowIcon(obs_icon)",
    "matching_receiver_count",
    "Add this existing receiver to current scene",
    "automatic_recovery_tick",
    "safety_timer_->setInterval(2000)",
    'obs_data_set_int(settings, "ndi_behavior", 0)',
)
for marker in required:
    if marker not in bridge:
        raise SystemExit(f"Compact UI/lifecycle marker is missing: {marker}")

print("Compact monitoring, OBS icon, and canonical-receiver audit passed")
