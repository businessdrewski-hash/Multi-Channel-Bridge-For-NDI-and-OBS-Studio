#!/usr/bin/env python3
"""Keep version strings, required files, and local Markdown links consistent."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
if version != "0.5.0-alpha1":
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

required = (
    ".github/workflows/build-windows.yml",
    "hotfix/bridge/sender-sync-core.cpp",
    "hotfix/bridge/sender-sync-core.h",
    "hotfix/bridge/av-governor.cpp",
    "hotfix/bridge/av-governor.h",
    "hotfix/bridge/multichannel-bridge.cpp",
    "hotfix/bridge/multichannel-bridge.h",
    "hotfix/scripts/patch_distroav.py",
    "tests/sender-sync-core-tests.cpp",
    "tests/av-governor-tests.cpp",
    "tests/test_parameter_paths.py",
    "tests/test_callback_safety.py",
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
