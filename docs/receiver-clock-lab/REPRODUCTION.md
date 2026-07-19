# Reproduction and evidence

## Known test configuration

- Receiver OBS: 32.1.2
- Receiver DistroAV baseline: 6.2.1
- Receiver source behavior: Keep Active
- Audio enabled: yes
- Baseline sync mode: NDI Source Timecode
- Baseline FrameSync: off
- Audio sample rate: 48 kHz
- Video observed in the diagnostic capture: 1920x1080, approximately 60 fps
- Topology: gaming/sender PC to streaming/receiver PC over NDI

Hardware, network adapter, switch, exact sender OBS audio/video settings, and output settings should be added before an
upstream report is submitted.

## Measured result

A continuous 14 minute 16 second valid window was captured without reconnects, setting changes, route contention, or
timestamp discontinuities.

| Boundary | First/last robust-window change |
| --- | ---: |
| NDI audio handoff into the diagnostic audio path | +0.0008 ms |
| Raw NDI video relative to raw NDI audio | +3.65 ms |
| OBS-selected video relative to DistroAV video output | -25.94 ms |
| Net uncorrected A/V deviation | -22.45 ms |

The raw-to-selected video boundary contributed more than the net drift; the smaller incoming raw A/V change opposed part
of it. DistroAV video timestamps advanced about 27.47 ms relative to receiver wall time while OBS-selected video advanced
about 1.69 ms. This is consistent with OBS consuming asynchronous video on the receiver cadence while the incoming media
timeline remains sender-paced.

The earlier diagnostic plugins established the measurement method, but they are deliberately absent from this branch.
The user also observed the same long-term drift with stock DistroAV before those plugins existed. The new three-mode test
is intended to reproduce that observation in one conventional DistroAV build.

The long capture also showed only about +0.0008 ms change from raw DistroAV audio into the split-input observation. That
rules out the Multichannel split as the cause in that run. It does not, by itself, constitute a controlled proof that every
sender channel-count/layout behaves identically; testing this lab with a normal stereo sender removes that remaining
variable.

## Test procedure

For each receiver mode:

1. Select the same NDI source and Keep Active behavior.
2. Enable receiver-clock diagnostics.
3. Apply the mode, which rebuilds the receiver.
4. Do not change source settings or reconnect during the run.
5. Record at least 45 minutes, or until 50 ms of visible drift is reached.
6. Export `receiver-clock-lab.csv`.
7. Analyze it with `tools/analyze-receiver-clock-log.py`.

Run the three modes in the order shown and restart the test timer after applying each mode. Do not run Sync Guardian or a
corrective Multichannel audio filter during the controlled comparison.

The receiver-paced mode passes the experiment if selected-video versus filtered-audio slope remains statistically near
zero and does not grow with session duration.
