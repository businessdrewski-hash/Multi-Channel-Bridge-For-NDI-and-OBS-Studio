# Troubleshooting

## Setup EXE fails

Close OBS and run setup as Administrator. Review:

```text
C:\ProgramData\Multichannel Bridge for DistroAV\Installer\install-script.log
C:\ProgramData\Multichannel Bridge for DistroAV\install.log
```

## Dock is missing

Search the OBS log for:

```text
[multichannel-bridge] Registered receiver proxy sources
[multichannel-bridge] Dock initialized
```

The dock is under **Docks -> Multichannel Bridge for DistroAV**.

## Two DistroAV menus

OBS is loading more than one `distroav.dll`. Re-run setup or search Program Files, ProgramData, Roaming AppData, and Local AppData. Only one active copy should remain for the OBS installation being used.

## Sender has video but no bridge audio

- Select Gaming PC / Sender.
- Use two different tracks.
- Route sources to those tracks in Advanced Audio Properties.
- Use 48 kHz on both PCs.
- Enable DistroAV Main Output.
- Confirm `Paired` rises.

A sender callback delta near 21.333 ms at 48 kHz can be normal for the two OBS mixes.

## Sender audio is stale, frozen, or suddenly offset

Use the smallest recovery first:

1. Click **Re-anchor sync** to flush the fixed audio queues and begin a new timing epoch.
2. If the sender or receiver still appears frozen, click **Restart Bridge** or use **Tools -> Restart Multichannel NDI Sender**.
3. Confirm the sender returns to `ACTIVE` and `Paired` begins rising again.

The restart action recreates only the Multichannel DistroAV Main Output and preserves the selected OBS tracks. It has a two-second cooldown to prevent accidental restart loops.

`Oversized blocks` should remain zero. A nonzero value means OBS delivered more than the fixed 4096-frame safety ceiling. `Callback contention drops` should also remain zero; a nonzero value means an unexpected overlapping selected-track callback was discarded instead of blocking OBS audio.

## Receiver has video but no split audio

- Select Stream PC / Receiver.
- Use one normal DistroAV NDI Source with audio enabled and Source Behavior set to Keep Active.
- Select that OBS source in the dock.
- Create/repair both split sources.
- Confirm four channels are detected and both proxy sources are active.
- If another scene needs the receiver, use **Add this existing receiver to current scene**. Do not create a second independent receiver for the same NDI sender.

## Governor remains WARMING UP or VERIFYING

- Click **Restore recommended settings**, then Apply.
- Confirm Frame Sync is off and Source Timecode is selected.
- Confirm the sender is this custom bridge Main Output.
- Confirm both split sources are active.
- Restart Main Output and the receiver source after upgrading both PCs.

The initial trusted reference requires at least 12 sane observations over five stable seconds. After a fault, two seconds of samples are quarantined before a five-second recovery candidate is compared with that trusted reference. The adapter keeps normal DistroAV audio/video live during this process.

## Governor enters HOLDING

The live status and flight recorder identify the reason:

- `video stall`: video stopped arriving beyond the configured threshold;
- `audio/video timestamp jump`: a large forward or backward source-time discontinuity;
- `timestamp repeated/backward`: a non-monotonic frame or block;
- `A/V deviation exceeded`: movement beyond the hard limit from the learned baseline;
- `shared playout depth left safe range`: the mapped OBS output timeline became implausibly far ahead of or behind local arrival time.

Copy **Diagnostics** and **A/V flight recorder** before changing settings. Fault samples are excluded from baseline and drift learning.

## Audio/video briefly cuts during a fault

The governor now fails open during warm-up, holding, verification, and failure, so its own timing decisions should not blank or mute the feed. If a cut remains, capture diagnostics: it is more likely in the sender, NDI transport, receiver, or OBS source lifecycle than an intentional governor gate.

## Governor says NEEDS ATTENTION

The recovered timing did not match the trusted reference, or more than three recoveries occurred within five minutes. Correction is bypassed and normal DistroAV output remains live. The dock attempts one in-place receiver reconnect; if the warning remains, follow its suggested action and export diagnostics before a manual reconnect or sender restart.

## Video correction remains at the configured maximum

The measured source clocks are continuing to diverge beyond the correction range. Do not simply raise the value indefinitely. Copy the flight recorder and inspect audio devices, sender timing, network interruptions, and source restarts. The governor reports the limit instead of chasing the drift indefinitely.

## Fixed delay feels too high

The default 120 ms is deliberately conservative. Reduce **Shared playout delay** gradually while testing. A lower value reduces latency but leaves less room for arrival jitter. The current minimum is 40 ms. Lower it gradually while watching playout-depth and recovery counters; very small values leave less room for arrival jitter.

## Source settings are not recommended

Enable automatic configuration and Apply, or manually set:

```text
NDI Frame Sync: Off
Sync mode: Source Timecode
Audio: Enabled
Source behavior: Keep Active
```

The governor bypasses enforcement when these conditions are not met rather than risking false blocks.

## Old missing-plugin warning

The bridge is integrated into `distroav.dll`; it does not use `ndi-multichannel-bridge.dll`. Re-run setup with stale Plugin Manager cleanup enabled.

## Uninstall fails

Close OBS and review the uninstall logs under ProgramData. The helper avoids overwriting a DistroAV DLL that changed after installation unless force restoration is explicitly requested.
