# Downstream Sync Core 2.0

The legacy filename is retained for existing links. Version 0.6 replaces the old receiver-side video governor with a video-master, linked-audio correction system.

## What it measures

The earlier implementation observed audio and video before their OBS source paths. That could report a stable relationship even while the final mixer output accumulated long-run drift.

Sync Core 2.0 observes:

1. the canonical DistroAV video timestamp through a private asynchronous-video filter;
2. the raw four-channel audio timestamp at the receiver handoff, before OBS receives either split proxy;
3. the linked frame/timestamp correction actually submitted to both split outputs.

The observations are projected to one monotonic wall-clock instant and median-filtered. A five-second stable window establishes the trusted reference. Later 30-to-120-second trend windows estimate native audio clock error, but they do not replace that reference. This means a fifth or eighth analysis window still reports cumulative movement from the first trusted lock.

## What it changes

Video is the master clock and every received video timestamp passes through unchanged.

When downstream evidence reaches at least 75% confidence, the core applies one shared PPM command to the four-channel packet before either proxy enters OBS. Positive video-minus-audio movement means audio is falling behind; the source handoff emits slightly fewer frames to move both audio buses forward together. A small catch-up component removes an existing offset, then the steady command converges on the measured native clock error.

Desktop/game and microphone share the same output frame count, timestamp, and fractional-frame accumulator, so they cannot drift relative to each other. The legacy OBS audio-filter type is inert and removed after source loading.

## Recommended defaults

```text
Enabled:                         yes
Automatic source configuration:  yes
Maximum linked audio correction: 1000 ppm
Correction slew:                  100 ppm/sec
Correction dead zone:               4 ms
Baseline learning window:        5000 ms
Drift analysis window:         120000 ms
Minimum drift observation:      30000 ms
NDI Frame Sync:                    off
DistroAV sync mode:                Source Timecode
NDI source behavior:               Keep Active
```

The 1000 ppm ceiling is primarily available to remove an existing offset. A steady 200 ms drift over 2.5 hours is about 22 ppm, so normal long-run correction should be far smaller.

## States

- `BYPASSED`: automatic correction is disabled.
- `LEARNING`: collecting the initial stable reference.
- `LOCKED`: the trusted reference is valid; trend measurement and correction are active.
- `VERIFYING`: a timestamp incident occurred; new samples are quarantined, then compared with the retained reference.
- `NEEDS ATTENTION`: recovered timing differs materially from the trusted reference; automatic correction is held at neutral pending reconnect.

## Recovery

Backward timestamps, repeated clock movement, or a greater-than-50 ms timestamp-versus-wall discontinuity start a two-second quarantine. If NDI itself remains alive, the first baseline and corrected output timeline are retained while the raw input clock is evaluated.

Any complete NDI receiver restart, whether manual, configuration-driven, or the single automatic recovery attempt, deliberately discards the old baseline, trend samples, PPM command, fractional remainder, accumulated correction, and last clock observations. The restarted feed is treated as the expected-sync condition and learns a new trusted reference after the brief cut.

After quarantine, five stable seconds must agree with the trusted reference within 40 ms. If not, the plugin leaves output live, stops correction, and requests attention rather than accepting the fault as a new normal.

## Real-time safety

- one fixed-size set of four-channel interpolation buffers owned by the receiver router;
- no allocation, logging, file access, UI work, or mutex wait in the audio callback;
- bounded linear interpolation only when the fractional command crosses a frame boundary;
- raw source packets are submitted unchanged while correction is disabled or a block exceeds the fixed safety capacity;
- the controller and trend regression run on a 250 ms Qt timer, even while the dock is hidden.

## Diagnostics

The compact dock reports raw audio movement, applied PPM, remaining corrected movement, and a nearby **RESTART NDI** action. The expanded Numbers panel adds the OBS-facing relation, trusted reference, native drift, packet age, and adjusted frames. **Export diagnostics** creates:

- `bridge-status.txt`
- `downstream-sync.csv`
- `README.txt`

## Limitations

- This is clock correction, not hardware genlock.
- Timestamps cannot prove that the content inside a decoded frame is semantically correct.
- Lost or corrupted media cannot be reconstructed.
- The alpha still requires long-duration testing in real Windows/OBS/DistroAV installations.
