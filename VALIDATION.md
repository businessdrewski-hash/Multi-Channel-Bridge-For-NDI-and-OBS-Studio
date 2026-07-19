# Validation notes for v0.6.0-alpha3

Completed in the source-generation environment:

- Python patcher syntax validation
- GitHub Actions workflow YAML parse validation
- Sender Sync Core 2.0 C++17 compilation with `-Wall -Wextra -Werror`
- Sender tests for canonical mix timestamps, 10,000-cycle bounded pairing, backward timestamp recovery, manual re-anchor, missing-track silence fallback, oversized input rejection, and the one-megabyte state-size ceiling
- Downstream Sync Core 2.0 C++17 compilation and a deterministic 2.5-hour/200 ms late-audio simulation
- Static sender and receiver audio-callback audits rejecting dynamic containers, allocation/growth calls, mutex waits, UI work, file work, and callback logging
- Parameter-path audit covering defaults, reads/runtime paths, and writes for all 16 saved settings
- Repository version and required-file checks
- Windows PowerShell 5.1 execution test for empty and populated installer-state serialization

Design properties enforced by code and release tests:

- Sender sample storage is fixed after construction.
- Sender raw-audio callbacks never wait for another callback.
- UI/control actions use atomic generation requests.
- Sender queues have a fixed four-block ceiling.
- Received video timestamps pass through unchanged and remain the master clock.
- Native audio drift is measured before correction from the first trusted reference.
- Desktop/game and microphone use one linked PPM correction and fixed interpolation storage.
- A trusted A/V reference cannot be replaced by a mismatched post-fault candidate.
- Recovery samples are quarantined and gradual drift requires at least 30 seconds of evidence.
- Mismatched recovery enters needs-attention bypass instead of accepting the fault as a new baseline.
- Receiver proxy sources are audio-active and explicit repair clears mute/zero-volume/empty-routing states.
- Existing DistroAV receiver sources expose an in-place reconnect procedure and only one automatic reconnect is attempted during fail-safe recovery, including while the dock is hidden.
- The receiver configuration prefers one canonical Keep Active source that is shared into other scenes by reference.
- The compact monitor exposes health, rushing/dragging direction, a brief recommendation, and collapsible exact numbers.
- Floating bridge windows inherit the OBS application icon.
- Monitoring peak scans and the dock timer stop while the dock is hidden.
- Receiver resampling storage is fixed-size and its callback has no allocation or mutex wait.

Not completed in this environment:

- Full Windows link/load test against OBS and DistroAV
- Inno Setup EXE compilation
- Multi-hour live NDI validation on two physical PCs
- Fault injection against real capture, USB audio, and network hardware

The GitHub Actions Windows job is the authoritative compile and packaging test. Production use should follow long-duration recording tests on the target computers before a live stream.
