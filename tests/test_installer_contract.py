#!/usr/bin/env python3
"""Fail the release if EXE delivery or installer diagnostics regress."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
workflow = (ROOT / ".github/workflows/build-windows.yml").read_text(encoding="utf-8")
installer = (ROOT / "installer/MultichannelBridge.iss").read_text(encoding="utf-8")
helper = (ROOT / "tools/Install-MultichannelBridge.ps1").read_text(encoding="utf-8")

workflow_markers = (
    "publish_release:",
    "INSTALLER-EXE",
    "if-no-files-found: error",
    "Installer does not have a Windows PE MZ header",
    "gh release create",
    "Multichannel-Bridge-for-DistroAV-Setup-v${{ steps.version.outputs.version }}.exe",
)
for marker in workflow_markers:
    if marker not in workflow:
        raise SystemExit(f"Windows EXE delivery contract is missing: {marker}")

installer_markers = (
    "SetupMutex=MultichannelBridgeForDistroAVSetup",
    "-ResultPath",
    "LoadStringsFromFile(ResultPath",
    "The previous DistroAV files were restored when possible.",
)
for marker in installer_markers:
    if marker not in installer:
        raise SystemExit(f"Installer error-reporting contract is missing: {marker}")

helper_markers = (
    "[string]$ResultPath",
    "Set-InstallStage 'preflight checks'",
    "pending-install-rollback",
    "Write-InstallResult $failureMessage",
    "Rollback completed.",
)
for marker in helper_markers:
    if marker not in helper:
        raise SystemExit(f"Install helper safety contract is missing: {marker}")

print("Installer and EXE delivery audit passed")
