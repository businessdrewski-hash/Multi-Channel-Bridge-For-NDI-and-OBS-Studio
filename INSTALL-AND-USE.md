# Install and use — NDI Multichannel Bridge 0.2.0

Install the same ZIP on both PCs. Close OBS and copy the ZIP contents into the OBS installation folder so `obs-plugins` and `data` merge with the existing folders. Keep normal DistroAV installed if desired, but do not run DistroAV Main Output for the same feed while this bridge sender is running.

## Gaming PC

1. Open OBS → Tools → NDI Multichannel Bridge.
2. Choose **Gaming PC — send**.
3. Open Edit → Advanced Audio Properties.
4. Put desktop/game audio on **Track 5**.
5. Put microphone audio on **Track 6**.
6. In the bridge, leave the defaults Track 5 and Track 6, choose an NDI name, and click **Start sender**.

The sender uses one NDI connection: video plus four planar audio channels. The audio assembler now accepts and realigns OBS track callbacks that are one full 1024-sample block apart (about 21.333 ms at 48 kHz), which fixes the all-audio-dropped failure in 0.1.x.

## Stream PC

1. Open OBS → Tools → NDI Multichannel Bridge.
2. Choose **Stream PC — receive**.
3. Enter the exact full NDI source name shown by the sender, normally `COMPUTERNAME (YourName)`.
4. Click **Create video + desktop + mic sources**.

The plugin adds three OBS sources to the current scene. They share one receiver and one mapped clock:

- Video
- Desktop/Game, NDI channels 1–2
- Microphone, NDI channels 3–4

Mute/remove old separate NDI desktop and mic sources while testing to avoid duplicates.

## Diagnostics

The control window shows the active computer role and basic state. For deeper diagnostics use Help → Log Files → View Current Log and search for `[NDI Multichannel Bridge]`.
