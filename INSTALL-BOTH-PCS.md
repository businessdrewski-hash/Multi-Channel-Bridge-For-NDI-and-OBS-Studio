# Install and use on both PCs

## 1. Build once on GitHub

1. Replace the repository contents with this source package, including the hidden `.github` folder.
2. Open **Actions → Build Multichannel Bridge for DistroAV (Windows)**.
3. Run the workflow.
4. Download `Multichannel-Bridge-for-DistroAV-v0.3.1-alpha-Windows-x64` after it succeeds.
5. Extract the downloaded GitHub artifact.

The artifact contains:

- `Multichannel-Bridge-for-DistroAV-Setup-v0.3.1-alpha.exe` — recommended installer
- `Multichannel-Bridge-for-DistroAV-v0.3.1-alpha-Portable-Windows-x64.zip` — portable/debug fallback
- `SHA256SUMS.txt`

## 2. Install the same EXE on both PCs

1. Close OBS completely.
2. Verify the setup EXE against `SHA256SUMS.txt`.
3. Run the setup EXE as Administrator.
4. Select the root OBS folder, normally `C:\Program Files\obs-studio`.
5. Leave legacy Plugin Manager cleanup enabled unless there is a reason not to reset a stale v0.2 entry.
6. Repeat with the exact same setup EXE on the other PC.
7. Start OBS on both PCs.

The installer:

- backs up the pre-bridge DistroAV DLL and data once;
- removes the obsolete standalone `ndi-multichannel-bridge.dll`;
- disables duplicate DistroAV copies in ProgramData and the current user's plugin folders;
- installs and hash-verifies the custom `distroav.dll`;
- registers a normal Windows uninstaller;
- preserves the original backup across bridge upgrades.

Because the EXE is not code-signed by default, Windows may show **Unknown publisher**. Use the supplied SHA-256 file to verify it.

## 3. Gaming PC setup

1. Open **Docks → Multichannel Bridge for DistroAV**.
2. Select **Gaming PC — send video + two stereo OBS tracks**.
3. Tick the role-confirmation box.
4. Leave Track A/Track B at **5/6**, or choose two different OBS tracks.
5. Click **Apply role and settings**.
6. In **Edit → Advanced Audio Properties**:
   - route desktop/game/program audio to Track A;
   - route microphone audio to Track B;
   - do not use **Monitor Only (mute output)** for either mix.
7. Remove old separate DistroAV audio-only filters while testing the combined feed.
8. Open **Tools → DistroAV Output Settings** and enable Main Output.
9. Confirm the dock shows **ACTIVE**, both meters move, and `Paired` continuously rises.

Default mapping:

- OBS Track 5 L/R → NDI channels 1/2
- OBS Track 6 L/R → NDI channels 3/4

## 4. Stream PC setup

1. Open **Docks → Multichannel Bridge for DistroAV**.
2. Select **Stream PC — receive and split the four NDI channels**.
3. Confirm the role and apply it.
4. Add one normal **DistroAV NDI Source** and select the gaming-PC Main Output.
5. Keep that source's NDI audio enabled.
6. In the bridge dock, refresh and select the OBS source by its OBS source name.
7. Keep original-audio suppression enabled.
8. Click **Create / repair split audio sources in current scene**.
9. Confirm these mixer sources appear independently:
   - `MCB Desktop / Game`
   - `MCB Microphone`
10. Route them to stream and recording tracks normally.

Begin testing with NDI Frame Sync disabled on the combined source. Compare with it enabled only when there is a specific reason to reclock the feed.

## 5. Healthy status

### Gaming PC

- `Sender active: yes`
- `Paired` rising continuously
- `Discarded: 0`
- `Silence fallback: 0`
- queues returning to `0 / 0`

### Stream PC

- `Receiver attached: yes`
- split outputs ready and active
- `Detected channels: 4`
- packet count rising
- missing program/mic counters at zero

## 6. Uninstall

Close OBS and use **Settings → Apps → Installed apps → Multichannel Bridge for DistroAV → Uninstall**. The uninstaller restores the pre-bridge DistroAV backup when available. Duplicate DistroAV copies disabled during installation are intentionally not restored automatically, because doing so would recreate duplicate plugin loading; their backups remain under ProgramData for manual recovery.
