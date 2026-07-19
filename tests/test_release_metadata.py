#!/usr/bin/env python3
"""Keep version strings, required files, and local Markdown links consistent."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
if version != "0.6.0-alpha5":
    raise SystemExit(f"Unexpected VERSION: {version}")

checks = {
    "hotfix/bridge/multichannel-bridge.cpp": f'kVersion = "{version}"',
    "hotfix/scripts/patch_distroav.py": f"v{version}",
    "installer/MultichannelBridge.iss": f'#define AppVersion "{version}"',
    "tools/Install-MultichannelBridge.ps1": f"$version = '{version}'",
    "README.md": f"Current version: **{version}**",
    "RELEASE-NOTES.md": f"# v{version} release notes",
    "VALIDATION.md": f"# Validation notes for v{version}",
}
for relative, marker in checks.items():
    text = (ROOT / relative).read_text(encoding="utf-8")
    if marker not in text:
        raise SystemExit(f"Version marker missing from {relative}: {marker}")

installer = (ROOT / "installer/MultichannelBridge.iss").read_text(encoding="utf-8")
if '#define AppNumericVersion "0.6.0.0"' not in installer:
    raise SystemExit("Installer numeric version is not 0.6.0.0")
for optional_doc in ("INSTALL-BOTH-PCS.md", "RELEASE-NOTES.md", "AV-GOVERNOR.md", "UPSTREAM-NOTES.md", "ROADMAP.md"):
    matching_lines = [line for line in installer.splitlines() if optional_doc in line and line.startswith("Source:")]
    if len(matching_lines) != 1 or "skipifsourcedoesntexist" not in matching_lines[0]:
        raise SystemExit(f"Optional installer document is not safely skippable: {optional_doc}")

workflow = (ROOT / ".github/workflows/build-windows.yml").read_text(encoding="utf-8")
for marker in ("$optionalDocs = @(", "$requiredPackagePaths = @(", "Required staged installer input is missing"):
    if marker not in workflow:
        raise SystemExit(f"Workflow staging guard is missing: {marker}")

required = (
    ".github/workflows/build-windows.yml",
    "hotfix/bridge/sender-sync-core.cpp",
    "hotfix/bridge/sender-sync-core.h",
    "hotfix/bridge/av-governor.cpp",
    "hotfix/bridge/av-governor.h",
    "hotfix/bridge/downstream-sync-core.cpp",
    "hotfix/bridge/downstream-sync-core.h",
    "hotfix/bridge/multichannel-bridge.cpp",
    "hotfix/bridge/multichannel-bridge.h",
    "hotfix/scripts/patch_distroav.py",
    "tests/sender-sync-core-tests.cpp",
    "tests/av-governor-tests.cpp",
    "tests/downstream-sync-core-tests.cpp",
    "tests/test_parameter_paths.py",
    "tests/test_callback_safety.py",
    "tests/test_installer_contract.py",
    "tests/test-installer-state.ps1",
    "tests/test_receiver_safety.py",
    "tests/test_governor_safety.py",
    "tests/test_compact_ui.py",
)
for relative in required:
    if not (ROOT / relative).is_file():
        raise SystemExit(f"Required release file is missing: {relative}")

for markdown in ROOT.glob("*.md"):
    text = markdown.read_text(encoding="utf-8")
    for target in re.findall(r"\[[^\]]+\]\(([^)]+)\)", text):
        if "://" in target or target.startswith("#"):
            continue
        clean = target.split("#", 1)[0]
        if clean and not (markdown.parent / clean).exists():
            raise SystemExit(f"Broken local link in {markdown.name}: {target}")

print("Release metadata audit passed")
