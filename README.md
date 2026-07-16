# NDI Multichannel Bridge 0.2.0

A single OBS plugin installed on both PCs. Select **Gaming PC** to send one NDI video stream with two discrete stereo pairs, or **Stream PC** to receive that stream and create separate video, desktop/game, and microphone OBS sources on one shared receiver clock.

See [INSTALL-AND-USE.md](INSTALL-AND-USE.md).

## 0.2.0 improvements

- One identical package for sender and receiver PCs.
- Saved per-PC role selector and simplified role-specific UI.
- Default Track 5 → channels 1–2 and Track 6 → channels 3–4.
- Receiver bundle creation with one shared NDI receiver/timestamp mapper.
- Audio queue realignment for the observed 21.333 ms OBS callback offset.
- Larger bounded queues, stale-block dropping, and matching/realignment counters in the core.
- Cleaner status guidance and unified GitHub Actions package.

Experimental; not affiliated with DistroAV, OBS, or Vizrt NDI AB. GPL-2.0-or-later.
