# Architecture

## Sender

The sender is an OBS raw multitrack output (`OBS_OUTPUT_AV | OBS_OUTPUT_MULTI_TRACK`). OBS supplies:

- BGRA program frames through `raw_video`
- planar-float stereo blocks for each selected OBS mixer through `raw_audio2`

`AudioPairAssembler` matches the two selected mixer callbacks by timestamp. It then creates one four-channel planar-float NDI audio frame:

```
NDI ch 0 = Track A left
NDI ch 1 = Track A right
NDI ch 2 = Track B left
NDI ch 3 = Track B right
```

Video and audio are submitted to one NDI sender using synthesized NDI timecodes.

## Receiver

One `ReceiverHub` exists per NDI source name. It creates exactly one NDI receiver and publishes frames to weak OBS-source subscribers:

- video subscribers receive the video frame
- pair-0 subscribers receive channels 0-1 as stereo
- pair-2 subscribers receive channels 2-3 as stereo

A single `TimelineMapper` converts the NDI 100 ns timeline into the receiver PC's local monotonic OBS nanosecond timeline. That mapper is shared by video and both audio outputs.

## Why not three receivers?

Three receiver instances may have independent buffering/reconnect state. The hub architecture ensures every output comes from the same capture call sequence and uses the same timeline anchor.
