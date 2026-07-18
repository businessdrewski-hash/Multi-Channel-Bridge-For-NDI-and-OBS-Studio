# v0.5.1-alpha1 release notes

## Trusted recovery and drift safety

- Keeps the last trusted A/V reference across discontinuities and receiver reconnects.
- Requires five stable seconds before accepting the initial reference or a recovery candidate.
- Quarantines two seconds of observations after a fault so a jump cannot contaminate baseline or drift calculations.
- Rejects a recovered offset that differs materially from the trusted reference instead of silently declaring it normal.
- Requires at least 30 seconds of persistent drift evidence, with entry/exit hysteresis, before pacing video timestamps.
- Reports when correction reaches its safe limit.
- Performs one automatic in-place receiver reconnect even when the dock is hidden, then fails open and stops automatic interference if safe recovery cannot be verified or recovery repeats too often.

## Compact monitoring and receiver QOL

- Replaces the wall of governor statistics with a compact color-coded health banner, plain-language rushing/dragging direction, track meters, and a one-line suggested action.
- Keeps exact values available in a separately collapsible **Numbers** panel and setup controls in a separately collapsible **Setup** panel.
- Renames technical status text around trusted reference, clock-domain offset, recovery, and verification.
- Detects multiple DistroAV receiver objects for the selected sender and warns that only one should own the split audio path.
- Adds **Add this existing receiver to current scene**, which reuses the canonical OBS source rather than opening another NDI connection.
- Applies Keep Active alongside Frame Sync off, Source Timecode, and audio enabled in recommended receiver settings.
- Makes floating bridge windows inherit OBS's normal application icon instead of the generic blank-page icon.

## Protected diagnostics

- Splits the fixed flight recorder into protected critical-event capacity and rate-limited telemetry capacity.
- Prevents routine correction updates from overwriting the jump, hold, verification, lock, or failure evidence needed for diagnosis.
- Adds trusted reference, recovery candidate, clock-domain offset, and fail-safe state to CSV export.

## Buildfix3 installer correction

- Fixed a Windows PowerShell 5.1 incompatibility while saving `install-state.json`.
- Explicitly converts the tracked duplicate-installation list to an array before JSON serialization, avoiding the post-verification `Argument types do not match` failure.
- Retains the verified buildfix2 rollback path; a failed attempt restores the DistroAV files that were present immediately before installation.

## Buildfix 2

- Promotes the Windows installer EXE to its own required build artifact.
- Manual workflow runs can publish the EXE, checksum, portable ZIP, and source patch directly as a GitHub prerelease.
- Verifies the installer file has a Windows PE `MZ` header before publishing it.
- Replaces the generic install-helper failure with the exact failed stage and error message.
- Writes `install-result.txt` beside the full installer log for quick diagnosis.
- Creates a short-lived snapshot of the immediately previous DistroAV DLL and data, restoring them if file installation or verification fails.
- Validates the packaged DLL and payload before starting OBS file changes.
- Reverts raw game-PC OBS timestamps as exported NDI timecodes and restores DistroAV's proven synthesized transport clock.
- Makes receiver timing protection fail open during warm-up, faults, and re-locks so it cannot create a black or silent feed.
- Adds an in-place DistroAV receiver reconnect procedure so stale upgraded sources no longer need to be deleted and recreated.
- Marks the two split proxy sources audio-active and repairs muted, zero-volume, or unrouted tracks when the user explicitly runs Create / repair.

## Buildfix 1

- Marked supplemental Markdown documents as optional installer inputs, so a missing `AV-GOVERNOR.md` cannot abort Inno Setup compilation.
- Added a staged-package manifest check that fails early and clearly if a runtime-critical DLL, script, readme, or license file is missing.

## Lightweight Sender Sync Core 2.0

- Replaced heap-backed sender queues with fixed-capacity, preallocated storage.
- Removed sender callback mutex waiting, per-block vectors, deque growth, and callback-time resizing.
- Added a non-blocking overlap guard that drops an unexpected concurrent callback instead of stalling OBS audio.
- Reduced the sender queue ceiling from twelve heap-backed blocks to four fixed blocks.
- Corrected the combined frame timestamp to the earlier selected-track callback, which represents the common OBS mix-interval start.
- Added bounded audio timestamp discontinuity detection and automatic fresh sender epochs.
- Added video timestamp continuity observation that requests an audio-queue re-anchor after backward movement or a multi-second timing gap.
- Kept silence fallback, now using permanently pre-zeroed storage.
- Peak scans now run only while the bridge dock is visible; the dock status timer also stops when hidden.

The two selected OBS tracks are already rendered by the same OBS audio engine before they reach the bridge. This version does not add a redundant sender resampler or estimate hardware drift from callback arrival time.

## Recovery and QOL

- Added **Re-anchor sync** to flush timing queues without destroying the NDI sender.
- Added **Restart Bridge** to recreate the Multichannel DistroAV Main Output with the same track mapping.
- Added `Tools -> Restart Multichannel NDI Sender` for quick access when the dock is closed.
- Restart requests are debounced for two seconds.
- UI actions hand off to the audio path through atomic generations rather than touching callback-owned storage.
- Added sender epoch, re-anchor, discontinuity, oversized-block, and callback-contention diagnostics.

## Stability audit

- Receiver callback routing no longer waits on the receiver UI/lifecycle mutex; it safely leaves the original path active if the lock is temporarily busy.
- Receiver fade buffers are fixed-size and cannot allocate during a recovery callback.
- All 20 saved settings are checked for default, load/runtime, and save paths.
- A callback-safety test rejects sender vectors, deques, mutexes, dynamic growth, UI work, file work, or callback logging.
- Sender tests cover 10,000 normal pair cycles, canonical timestamps, discontinuities, manual re-anchor, missing-track fallback, oversized-block rejection, and the one-megabyte state budget.

## Receiver behavior

A/V Governor 1.3 and the two split receiver sources remain available. The four-channel mapping remains unchanged:

```text
OBS Track A -> NDI channels 1-2 -> Desktop / Game
OBS Track B -> NDI channels 3-4 -> Microphone
```

## Build outputs

The GitHub Action builds:

```text
Multichannel-Bridge-for-DistroAV-Setup-v0.5.1-alpha1.exe
Multichannel-Bridge-for-DistroAV-v0.5.1-alpha1-Portable-Windows-x64.zip
Multichannel-Bridge-DistroAV-6.2.1.patch
SHA256SUMS.txt
```

The complete Windows DistroAV/OBS build is authoritative. This source-generation environment validates the standalone timing cores, patcher syntax, workflow syntax, settings paths, and callback-safety rules but cannot load the resulting Windows DLL into OBS.
