# v0.6.0-alpha5 release notes

## Alpha 5 source-level correction and hard reset

- Moves linked PPM resampling out of the two OBS audio filters and applies it once to the shared four-channel packet before either MCB proxy source enters OBS.
- Gives desktop/game and microphone the same corrected frame count, packet timestamp, fractional remainder, and accumulated adjustment by construction.
- Leaves the old filter type registered only as an inert compatibility shim, then removes every serialized legacy filter after OBS source loading.
- Prevents a filter from changing `audio->frames` or rebasing timestamps after the standard OBS mixer and meters begin processing a block, addressing the confirmed nonzero-PPM meter jumps.
- Makes every full NDI receiver restart a hard timing epoch reset for this personal build: baseline, trend history, correction command, fractional remainder, accumulated frames, and corrected packet timestamps are wiped.
- Learns the restarted receiver as the new expected-sync reference instead of carrying an old correction into the new baseline and falsely reporting approximately `-97 ms` of rushing audio.
- Retains Downstream Sync Core 2.0 as the measurement/controller layer; the shared source-level resampler is now its actuator.

## Alpha 4 receiver epoch controls

- Adds a compact **RESTART NDI** button beside the suggested action in the always-visible stream-PC monitor.
- A manual restart rebuilds the existing canonical DistroAV receiver in place, intentionally discards the old trusted reference, and learns a fresh baseline after the brief A/V cut.
- Applying any receiver-side bridge setting now performs exactly one receiver restart after all settings are committed and starts a clean timing epoch.
- Clears stale pre-restart Sync Core observations; alpha 5 additionally clears the actuator's accumulated correction and timestamps.
- Adds an always-visible summary of raw audio movement, applied PPM, and remaining corrected movement.
- Alpha 4 automatic recovery preserved its trusted reference. Alpha 5 supersedes this for the personal build: every complete NDI restart now rebaselines.

## Alpha 3 filter lifecycle correction

- Fixes linked audio clock filters accumulating on the two MCB proxy sources after each stream-PC OBS restart.
- Reconciles filters after OBS finishes restoring the source collection, removing every matching filter by source type rather than only one exact display name.
- Recreates exactly one correctly configured linked clock filter per MCB audio proxy.
- Adds a release check preventing regression to name-only filter cleanup.

## Alpha 2 UI correction

- Removes four obsolete legacy timing spin boxes that were still being constructed without layout positions.
- Fixes the visible `12 pairs` control overlapping the automatic audio drift correction title.
- Leaves the compact layout and the active downstream correction controls unchanged.

## Long-recording drift correction

- Replaces receiver video timestamp pacing with Downstream Sync Core 2.0.
- Observes canonical video downstream in OBS and raw audio at the shared receiver handoff, then explicitly accounts for the correction applied to the two proxy outputs.
- Keeps video as the master clock and leaves its timestamps unchanged.
- Measures native audio drift before correction and retains the first trusted reference across every later trend window.
- Applies one shared, slew-limited PPM command to desktop/game and microphone, preserving their relative timing.
- Uses one set of fixed preallocated four-channel interpolation buffers; the receiver audio callback does not allocate, log, touch UI, or wait on a mutex.
- Preserves accumulated corrected timing across raw input timestamp re-anchors and verifies the recovered output against the trusted reference.
- Adds a deterministic 2.5-hour simulation of 22.22 ppm audio error: 200 ms raw late drift is detected while corrected output remains near the trusted sync.

## Compact UI and defaults

- Keeps the existing Setup and Numbers toggles and the same compact monitor layout.
- Shows OBS-facing A/V, corrected change, trusted reference, native drift, and linked audio correction without adding a permanent wall of controls.
- Keeps expert controls collapsed by default.
- Recommended defaults are 1000 ppm maximum correction, 100 ppm/second slew, a 4 ms dead zone, a five-second baseline, and a 30-to-120-second evidence window.

## Windows workflow correction

- Checks the PowerShell installer-state script with `$?` rather than native-process `$LASTEXITCODE`.
- Prevents a successful serialization test from being reported as `Installer-state test failed with exit code` followed by an empty value.

## Trusted recovery and drift safety

- Keeps the last trusted A/V reference across in-place timestamp incidents that do not rebuild NDI.
- Requires five stable seconds before accepting the initial reference or a recovery candidate.
- Quarantines two seconds of observations after a fault so a jump cannot contaminate baseline or drift calculations.
- Rejects a recovered offset that differs materially from the trusted reference instead of silently declaring it normal.
- Requires at least 30 seconds of persistent drift evidence before changing the linked audio rate.
- Reports when correction reaches its safe limit.
- Performs at most one automatic in-place receiver restart even when the dock is hidden; that complete restart intentionally begins a fresh baseline in this personal build.

## Compact monitoring and receiver QOL

- Replaces the wall of governor statistics with a compact color-coded health banner, plain-language rushing/dragging direction, track meters, and a one-line suggested action.
- Keeps exact values available in a separately collapsible **Numbers** panel and setup controls in a separately collapsible **Setup** panel.
- Renames technical status text around trusted reference, clock-domain offset, recovery, and verification.
- Detects multiple DistroAV receiver objects for the selected sender and warns that only one should own the split audio path.
- Adds **Add this existing receiver to current scene**, which reuses the canonical OBS source rather than opening another NDI connection.
- Applies Keep Active alongside Frame Sync off, Source Timecode, and audio enabled in recommended receiver settings.
- Makes floating bridge windows inherit OBS's normal application icon instead of the generic blank-page icon.

## Downstream diagnostics

- Exports the trusted reference, raw cumulative deviation, corrected deviation, native drift, linked correction, observations, and adjusted-frame count.

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
- Receiver resampling buffers are fixed-size and cannot allocate during an audio callback.
- All 16 saved settings are checked for default, load/runtime, and save paths.
- A callback-safety test rejects sender vectors, deques, mutexes, dynamic growth, UI work, file work, or callback logging.
- Sender tests cover 10,000 normal pair cycles, canonical timestamps, discontinuities, manual re-anchor, missing-track fallback, oversized-block rejection, and the one-megabyte state budget.

## Receiver behavior

Downstream Sync Core 2.0 and the two split receiver sources are enabled by default. The four-channel mapping remains unchanged:

```text
OBS Track A -> NDI channels 1-2 -> Desktop / Game
OBS Track B -> NDI channels 3-4 -> Microphone
```

## Build outputs

The GitHub Action builds:

```text
Multichannel-Bridge-for-DistroAV-Setup-v0.6.0-alpha5.exe
Multichannel-Bridge-for-DistroAV-v0.6.0-alpha5-Portable-Windows-x64.zip
Multichannel-Bridge-DistroAV-6.2.1.patch
SHA256SUMS.txt
```

The complete Windows DistroAV/OBS build is authoritative. This source-generation environment validates the standalone timing cores, patcher syntax, workflow syntax, settings paths, and callback-safety rules but cannot load the resulting Windows DLL into OBS.
