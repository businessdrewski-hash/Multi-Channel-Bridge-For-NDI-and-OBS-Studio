# v0.5.0-alpha1-buildfix1 release notes

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

A/V Governor 1.2 and the two split receiver sources remain available. The four-channel mapping remains unchanged:

```text
OBS Track A -> NDI channels 1-2 -> Desktop / Game
OBS Track B -> NDI channels 3-4 -> Microphone
```

## Build outputs

The GitHub Action builds:

```text
Multichannel-Bridge-for-DistroAV-Setup-v0.5.0-alpha1-buildfix1.exe
Multichannel-Bridge-for-DistroAV-v0.5.0-alpha1-buildfix1-Portable-Windows-x64.zip
Multichannel-Bridge-DistroAV-6.2.1.patch
SHA256SUMS.txt
```

The complete Windows DistroAV/OBS build is authoritative. This source-generation environment validates the standalone timing cores, patcher syntax, workflow syntax, settings paths, and callback-safety rules but cannot load the resulting Windows DLL into OBS.
