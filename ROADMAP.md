# Roadmap

## Implemented through 0.5.0-alpha1-buildfix1

- Robust baseline learning and median jitter filtering
- High-confidence drift-rate estimation
- Frame-boundary, video-only timing correction
- Shared OBS-native playout timeline
- Atomic epoch recovery with audio fade boundaries
- Raw NDI-versus-OBS timing capture
- Diagnostic bundle export
- Reviewable DistroAV patch artifact
- Windows EXE installer workflow
- Fixed-capacity, preallocated Sender Sync Core 2.0
- Canonical mix-interval timestamps and automatic sender re-anchor
- One-click sender re-anchor and complete Bridge restart
- Hidden-dock monitoring suspension and callback-safety release gates

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
- PPM audio correction only if testing proves real residual clock drift that video pacing cannot safely handle
