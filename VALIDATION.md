# Validation notes for v0.5.0-alpha1-buildfix2

Completed in the source-generation environment:

- Python patcher syntax validation
- GitHub Actions workflow YAML parse validation
- Sender Sync Core 2.0 C++17 compilation with `-Wall -Wextra -Werror`
- Sender tests for canonical mix timestamps, 10,000-cycle bounded pairing, backward timestamp recovery, manual re-anchor, missing-track silence fallback, oversized input rejection, and the one-megabyte state-size ceiling
- A/V Governor 1.2 C++17 compilation and existing standalone recovery tests
- Static game-PC callback audit rejecting dynamic containers, allocation/growth calls, mutex waits, UI work, file work, and callback logging
- Parameter-path audit covering defaults, reads/runtime paths, and writes for all 20 saved settings
- Repository version and required-file checks

Design properties enforced by code and release tests:

- Sender sample storage is fixed after construction.
- Sender raw-audio callbacks never wait for another callback.
- UI/control actions use atomic generation requests.
- Sender queues have a fixed four-block ceiling.
- Receiver timing recovery fails open, preserves synthesized NDI transport timecodes, and cannot blank a live feed.
- Receiver proxy sources are audio-active and explicit repair clears mute/zero-volume/empty-routing states.
- Existing DistroAV receiver sources expose an in-place reconnect procedure.
- Monitoring peak scans and the dock timer stop while the dock is hidden.
- Receiver fade scratch storage is fixed-size.

Not completed in this environment:

- Full Windows link/load test against OBS and DistroAV
- Inno Setup EXE compilation
- Multi-hour live NDI validation on two physical PCs
- Fault injection against real capture, USB audio, and network hardware

The GitHub Actions Windows job is the authoritative compile and packaging test. Production use should follow long-duration recording tests on the target computers before a live stream.
