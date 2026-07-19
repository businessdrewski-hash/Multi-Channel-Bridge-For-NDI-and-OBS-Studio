# Roadmap

## Implemented through 0.6.0-alpha5

- Stable trusted-reference learning, sample quarantine, and median jitter filtering
- High-confidence drift-rate estimation after at least 30 seconds of persistent evidence
- Downstream OBS-facing video/audio measurement
- Video-master timing with linked PPM correction for both stereo audio buses
- Trusted-reference recovery verification with continuous corrected audio timing
- Compact downstream-sync diagnostic export
- Reviewable DistroAV patch artifact
- Windows EXE installer workflow
- Fixed-capacity, preallocated Sender Sync Core 2.0
- Canonical mix-interval timestamps and automatic sender re-anchor
- One-click sender re-anchor and complete Bridge restart
- Hidden-dock monitoring suspension and callback-safety release gates
- Compact color-coded monitoring with rushing/dragging direction, brief recommendations, and collapsible exact numbers
- Canonical Keep Active receiver reuse across scenes and duplicate-receiver warnings
- Normal OBS icon inheritance for floating bridge windows
- Explicit receiver timing epochs after settings changes and manual **RESTART NDI**
- Compact raw drift, applied PPM, and corrected-offset status
- Shared source-level four-channel PPM resampling with inert legacy-filter cleanup
- Hard rebaseline on every complete NDI receiver restart for the personal setup

## Next validation work

- Multi-hour 4K60 tests with Frame Sync disabled
- Fault injection: network interruption, source reconnect, game-capture rehook, missing audio track, and sender restart
- Compare raw NDI fields with converted OBS timestamps during real failures
- Tune baseline, confidence, and hold thresholds from recorded data
- Verify behavior across current OBS, NDI Runtime, and DistroAV releases

## Possible later features

- Up to six OBS stereo buses / twelve NDI audio channels
- Source UUID attachment rather than source-name attachment
- Scene-independent split sources
- Optional diagnostic upload bundle with privacy review
- Authenticode signing
- Optional higher-quality band-limited resampling if listening tests justify more CPU cost
