# AI-assisted investigation worklog

This experiment was developed through an AI-assisted, evidence-first workflow. The human operator supplied the real
two-computer OBS/NDI environment, repeated long-duration observations, and exported timing captures. The AI assisted with
source review, instrumentation, statistical decomposition, and implementation.

Representative initial questions were:

- Where does DistroAV/OBS lose A/V pacing if changing video timestamps does not stop the drift?
- Can every stage from NDI metadata through OBS-selected video and mixer audio be logged behind a checkbox?
- Does the drift occur before DistroAV output, in the multichannel split, or after `obs_source_output_video`?
- Can the core receiver issue be fixed instead of applying a downstream PPM correction?

The important methodological correction was to stop treating a near-zero calculated “corrected deviation” as proof. The
long capture was decomposed at every observed boundary. That showed the audio handoff was stable and the dominant changing
offset appeared between DistroAV video output and OBS-selected video. Reviewing stock DistroAV then showed that both direct
and existing FrameSync modes continue to emit sender-derived timestamps without a receiver-clock scheduler.

This branch is intentionally conventional DistroAV source code so maintainers can review the proposed behavior without
understanding or integrating the earlier experimental plugins.
