# A/V Governor 1.3

A/V Governor is an optional receiver-side timing subsystem inside the same custom DistroAV build as Multichannel Bridge.

## Goal

Prevent a healthy audio/video relationship from separating before DistroAV submits media to OBS. It distinguishes ordinary jitter, slow clock movement, and real discontinuities instead of treating every timing change as the same problem.

## How it works

The receiver hook sees both the raw NDI `timestamp`/`timecode` values and the OBS timestamps produced by DistroAV. Before `obs_source_output_audio` or `obs_source_output_video` is called, the governor:

1. projects the latest audio and video clocks to one common local instant;
2. median-filters short jitter and establishes a trusted fixed A/V reference only after at least five stable seconds;
3. maps both streams onto the same future OBS playout timeline, using OBS's native asynchronous source queues rather than copying 4K video frames into another custom buffer;
4. estimates persistent drift only after enough time and samples produce a high-confidence trend;
5. keeps audio sample-perfect and makes bounded, frame-boundary video timestamp corrections only for confirmed gradual drift;
6. detects stalls, backward/repeated timestamps, large jumps, unsafe playout depth, and excessive movement away from baseline;
7. preserves the trusted reference, quarantines post-fault samples for two seconds, and verifies a stable recovery candidate against that reference;
8. makes one in-place receiver reconnect attempt and then enters fail-safe bypass if the candidate is unsafe or recovery repeatedly fails;
9. exports protected critical events and rate-limited telemetry containing raw NDI timing, OBS timing, skew, drift, playout depth, corrections, and recovery events.

The core records which packets it would gate during a video stall or re-lock, but the OBS adapter fails open with the original DistroAV timestamps. This keeps the feed visible and audible while protection reacquires. Audio is not resampled, stretched, or PPM-adjusted.

## Recommended defaults

```text
Enabled:                         yes
Automatic source configuration:  yes
Shared playout delay:             120 ms
Hard A/V deviation limit:         120 ms
Video-stall hold threshold:       120 ms
Baseline learning window:        5000 ms
Drift analysis window:         120000 ms
Minimum drift observation:      30000 ms
Drift deadband:                     8 ppm
Gradual video correction:         yes
Maximum video correction:          40 ms
Video correction slew:           1000 ppm
Minimum recovery observations:     12
NDI Frame Sync:                    off
DistroAV sync mode:                Source Timecode
NDI source behavior:               Keep Active
```

## States

- `BYPASSED`: disabled or the source is not configured for Frame Sync off, Source Timecode, and audio enabled.
- `WARMING UP`: collecting initial timing observations.
- `VERIFYING`: quarantining recovery samples and comparing a stable candidate with the trusted reference.
- `FAILED`: correction is bypassed because safe recovery could not be verified; normal DistroAV output remains live.
- `LOCKED`: both paths are accepted on the shared playout timeline.
- `HOLDING`: a discontinuity or unsafe timing state was detected; internal protected-timeline decisions pause while the adapter leaves normal DistroAV output live.

## Diagnostics

The dock reports raw and filtered skew, learned baseline, baseline sample count, drift rate and confidence, audio/video playout depth, estimated video interval, correction amount, fades, blocked packets, raw NDI timing fields, epochs, and recoveries.

**Export diagnostics** creates a timestamped folder with:

- `bridge-status.txt`
- `av-governor-flight-recorder.csv`
- a short explanation file

The recorder uses separate fixed-size rings. Holds, drops, locks, fades, resets, and failures use protected critical capacity. Routine samples and correction telemetry are rate-limited and cannot overwrite those critical events.

## Performance

- no second NDI video sender;
- no copied or retained video payloads;
- no audio resampling;
- fixed-size timing windows and recorder rings;
- no per-packet logging;
- one short mutex around constant-bounded timing work;
- fade scratch buffers are fixed-size and never resized in a callback.

## Limitations

- The playout window relies on OBS asynchronous-source scheduling; it is not hardware genlock.
- Timestamps cannot prove that the visible and audible content itself is correct.
- Media lost before the receiver cannot be reconstructed.
- A severe sender, capture, network, or decoder failure may still cause a brief coordinated freeze or gap.
- This alpha still requires long-duration and fault-injection testing on Windows.
