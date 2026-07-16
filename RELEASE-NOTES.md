# v0.3.1-alpha release notes

## Installer release

- Added a proper Windows setup EXE built with Inno Setup.
- Added Windows Installed Apps / Add or Remove Programs registration and uninstall support.
- Added OBS-folder validation and a hard block while OBS is running.
- Added one-time backup of the pre-bridge DistroAV installation, preserved across upgrades.
- Added cleanup and backup of duplicate DistroAV copies in ProgramData, Roaming AppData, and Local AppData.
- Added removal of the obsolete standalone v0.2 bridge.
- Added optional cleanup of the stale OBS Plugin Manager record that previously caused a misleading missing-plugin warning.
- Added post-copy SHA-256 verification and a check that exactly one active `distroav.dll` remains in the common plugin locations.
- Added installer and uninstaller logs under ProgramData.
- Renamed the product-facing dock and documentation to **Multichannel Bridge for DistroAV** while preserving existing configuration keys and source IDs for upgrade compatibility.

## Existing bridge behavior

- One integrated custom DistroAV build is installed on both gaming and stream PCs.
- Sender mode packs two OBS stereo mixes into four NDI audio channels alongside DistroAV Main Output video.
- Receiver mode splits raw planar channels before OBS downmixes them.
- Bounded queues accept OBS's normal one-block callback phase and provide reported silence fallback.
- Diagnostics expose pairing, discards, fallback, queue depth, detected channels, packet age, missing pairs, and duplicate-audio suppression.

## Validation status

- The Python patcher passes syntax validation.
- Installer, workflow, and PowerShell scripts were structurally reviewed in the source-generation environment.
- GitHub Actions is the authoritative Windows compile and Inno Setup build test.
- Treat this as controlled-test alpha software and verify with a long local recording before live production.
