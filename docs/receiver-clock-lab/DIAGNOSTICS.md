# Diagnostic CSV

Enable **Capture receiver-clock diagnostics** in the NDI Source properties. The recorder samples atomic stage observations
every 250 ms and retains up to three hours in memory. It also writes and periodically flushes a live CSV, so a long run is
not lost if OBS exits before the export button is pressed. The OBS log prints its exact path as
`[receiver-clock-lab] Live diagnostics path: ...`.

Each source gets its own `receiver-clock-lab-<OBS source UUID>.csv` in DistroAV's OBS plugin configuration directory.
Enabling capture starts a new session and replaces that source's previous live file. Turning capture off closes the live
file while preserving the in-memory recorder. **Export receiver-clock-lab.csv** writes an additional snapshot with a
simple filename.

## Stages

- `capture_audio`: direct NDI receive result, or NDI FrameSync output in either FrameSync mode, with original metadata.
- `capture_video`: direct NDI receive result, or NDI FrameSync output in either FrameSync mode, with original metadata.
- `output_audio`: final timestamp and format immediately before `obs_source_output_audio`.
- `output_video`: final timestamp immediately before `obs_source_output_video`.
- `filtered_audio`: unchanged downstream OBS audio-filter observation.
- `selected_video`: unchanged observation after OBS's async `get_closest_frame` selection. OBS invokes raw async-video
  filters on the selected `cur_async_frame`, so this is specifically downstream of `obs_source_output_video`.

## Scheduler fields

The CSV includes receiver epoch, next audio/video deadlines, cumulative audio frames, receiver video ticks, deadline error,
catch-up counters, repeated/empty pull counts, NDI receive totals and drops, and NDI audio/video queue depths.

Derived projected relations compensate for the fact that audio and video callbacks are observed at different wall times.
The NDI API does not expose the frames internal to FrameSync before it selects/resamples them. The receive performance and
queue counters are therefore the closest public observation of that internal boundary. This limitation is explicit rather
than being inferred as a measured stage.

Diagnostics never change timestamps, sample counts, or frame selection.
