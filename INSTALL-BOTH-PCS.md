# Install on both PCs

Install the same `0.6.0-alpha5` package on the gaming PC and stream PC.

## Recommended EXE installation

1. Download `Multichannel-Bridge-for-DistroAV-Setup-v0.6.0-alpha5.exe`.
2. Close OBS completely.
3. Run setup as Administrator.
4. Select the root OBS folder, normally `C:\Program Files\obs-studio`.
5. Repeat on the other PC.
6. Open **Docks -> Multichannel Bridge for DistroAV** and choose the correct role.

The installer backs up DistroAV, removes obsolete bridge files, disables common duplicate DistroAV copies, verifies the installed DLL, and adds an uninstaller.

## Gaming PC

1. Select **Gaming PC / Sender**.
2. Confirm the role.
3. Use two different OBS tracks; defaults are 5 and 6.
4. Route game/program audio to Track 5 and mic to Track 6 in Advanced Audio Properties.
5. Apply settings.
6. Enable DistroAV Main Output.
7. Confirm `Paired` rises and `Discarded`, `Silence fallback`, `Oversized`, and `Contention` remain near zero.
8. Use **Re-anchor sync** for a timing-only refresh or **Restart Bridge** to recreate the complete sender.

## Stream PC

1. Add one normal DistroAV NDI Source for the gaming-PC Main Output. This is the canonical receiver.
2. Select **Stream PC / Receiver** in the bridge dock.
3. Select that OBS source.
4. Leave automatic audio drift correction and automatic source timing enabled.
5. Click **Create / repair split audio sources**.
6. Confirm `MCB Desktop / Game` and `MCB Microphone` appear separately.
7. Keep original-audio suppression enabled.
8. Confirm Downstream Sync Core reaches `LOCKED`.
9. For other scenes, select the same receiver and use **Add this existing receiver to current scene**. Do not create another independent DistroAV receiver object for the same sender.

Recommended receiver timing is applied automatically:

```text
NDI Frame Sync: Off
Sync mode: Source Timecode
Audio: Enabled
Source behavior: Keep Active
Linked audio correction: On
Maximum audio correction: 1000 ppm
Correction slew: 100 ppm/sec
Correction dead zone: 4 ms
Trusted reference time: at least 5 stable seconds
Post-fault quarantine: 2 seconds
Minimum drift evidence: 30 seconds
```

After setup, collapse **Setup** and leave the compact monitor visible. Expand **Numbers** only when you want exact timing values. Use **RESTART NDI** when you want the brief hard reset and a completely fresh baseline; applying receiver settings does the same once after all changes are committed. Use **Copy diagnostics** after any unexplained jump, stall, or meter event.

## Uninstall

Close OBS, then use **Windows Settings -> Apps -> Installed apps -> Multichannel Bridge for DistroAV -> Uninstall**. The pre-bridge DistroAV backup is restored when available.
