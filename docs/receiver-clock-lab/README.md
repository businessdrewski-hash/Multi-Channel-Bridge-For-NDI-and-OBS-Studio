# DistroAV Receiver Clock Lab

This branch is an isolated experiment based directly on DistroAV 6.2.1. It does not contain Multichannel Bridge,
Sync Guardian, linked audio filters, PPM correction, or sender track-combining code. The NDI source remains one normal
audio/video OBS source.

The experiment tests one hypothesis: DistroAV currently forwards a sender-paced NDI timeline into an OBS receiver whose
asynchronous video path is paced by the receiver clock. A shared receiver-clocked NDI FrameSync pull should prevent the
two timelines from diverging.

## Comparison modes

The NDI Source properties expose three modes in the same binary:

1. **Stock DistroAV: direct receive** — unchanged `recv_capture_v3` reference path.
2. **Stock DistroAV: existing FrameSync** — unchanged DistroAV 6.2.1 polling path.
3. **Receiver Clock Lab: OBS-paced FrameSync** — pulls audio and video on absolute receiver deadlines and assigns both
   outputs one receiver-clock epoch.

No mode applies an adaptive downstream sync correction. Changing the mode rebuilds the NDI receiver so each test begins
with a fresh timing epoch.

## What the new mode changes

- Audio is pulled at the OBS audio sample rate in receiver-scheduled blocks.
- Video is pulled at the OBS video cadence.
- Both output timestamps are derived from the same receiver epoch.
- Late video deadlines skip receiver ticks rather than accumulating schedule error.
- Late audio deadlines request a bounded larger block to catch up without changing the long-term timeline.
- Original NDI timestamp and timecode remain in diagnostics but do not schedule OBS in the receiver-paced mode.

## Current status

This is diagnostic pre-release code. It must be validated against the two stock reference modes before proposing an
upstream production change.

The package deliberately retains DistroAV's module and source IDs. It replaces stock DistroAV for a controlled test and
cannot be loaded beside stock DistroAV in the same OBS installation. It is fully separate from Multichannel Bridge and
Sync Guardian, which may remain uninstalled during this test.

See [REPRODUCTION.md](REPRODUCTION.md), [DIAGNOSTICS.md](DIAGNOSTICS.md), and [DESIGN.md](DESIGN.md).
