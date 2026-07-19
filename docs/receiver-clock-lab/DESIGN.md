# Receiver-clock design

## Existing DistroAV 6.2.1 behavior

The direct path waits for `recv_capture_v3` and immediately outputs whichever sender-paced audio or video frame arrives.
OBS timestamps are copied from the NDI timestamp or source timecode.

The existing FrameSync path creates one NDI FrameSync instance, but polls it in a loop with a fixed 5 ms sleep. It asks
for 1,024 audio samples, suppresses output until the returned source timestamp changes, and still copies the returned NDI
timestamp/timecode into OBS. This selects synchronized NDI data but does not establish a receiver-clock output cadence.

## Experimental behavior

One absolute receiver epoch is established after the NDI receiver and FrameSync objects are created. Audio and video have
independent deadlines derived from that same epoch:

```text
audio timestamp = epoch + cumulative output samples / OBS sample rate
video timestamp = epoch + receiver video tick * OBS frame interval
```

The NDI FrameSync API performs source selection/sample conversion. DistroAV owns only the receiver scheduler and the OBS
timestamps. There is no drift estimator or feedback controller.

## Why this is not Sync Guardian

Sync Guardian measured downstream error and altered one stream after the error accumulated. Receiver Clock Lab defines the
clock at ingestion. The expected rate conversion is a normal consequence of crossing between independent computer clocks,
and is delegated to NDI FrameSync before frames enter OBS.

## Upstreaming strategy

The production-sized change should remain confined to the NDI source receiver:

- a small receiver scheduler with unit tests;
- a source property selecting the legacy or receiver-clock path during evaluation;
- diagnostic hooks that can later feed DistroAV's network monitor;
- no sender changes and no OBS-core patch.

Once validated, the reference modes and verbose recorder can be reduced or removed, leaving the receiver-paced FrameSync
path as a focused DistroAV change.
