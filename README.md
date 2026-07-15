# NDI Multichannel Bridge for OBS Studio

**Experimental v0.1.0 alpha**

NDI Multichannel Bridge sends the OBS program video and two independently routed stereo mixes inside **one NDI A/V stream**:

- NDI channels 1-2: desktop/game mix
- NDI channels 3-4: microphone mix
- Video and all four audio channels: one NDI sender and one timing domain

On the receiving PC, the plugin captures that stream once and fans it out as:

- `NDI Multichannel Video`
- `NDI Multichannel Audio Pair` for channels 1-2
- `NDI Multichannel Audio Pair` for channels 3-4

The receiver sources share one receiver thread and one timestamp mapper instead of opening three independent NDI receivers.

## Why this exists

Separate NDI video, desktop-audio, and mic sources can reconnect, buffer, and drift independently. NDI supports arbitrary discrete audio-channel counts, and DistroAV 6.2.1 already demonstrates four important pieces this plugin builds on: NDI v3 planar-float audio frames, synthesized NDI timecodes, raw OBS A/V output, and multichannel NDI receive support.

## Sender setup

1. Install the plugin on the gaming/sender PC.
2. In **Advanced Audio Properties**, place desktop/game audio on one otherwise-unused OBS track and mic on another.
3. Open **Tools > NDI Multichannel Bridge**.
4. Choose the desktop/game track for NDI channels 1-2.
5. Choose the mic track for NDI channels 3-4.
6. Start the sender.

The selected tracks should normally contain only the intended sources. OBS Track 1 can still be your ordinary stream mix; for example, use Track 5 for desktop and Track 6 for mic.

## Receiver setup

1. Install the plugin on the streaming/receiver PC.
2. Open **Tools > NDI Multichannel Bridge**.
3. Enter the exact sender NDI source name.
4. Click **Create video + desktop + mic sources**.
5. Route the two resulting audio sources to Twitch and recording tracks normally.

Do not add three separate DistroAV receivers for this same feed. The built-in receiver bundle is what keeps the video and both audio pairs on one capture/timestamp path.

## DistroAV relationship

This is an independent GPL OBS plugin designed to coexist with DistroAV and use the same installed NDI Runtime. It does not modify DistroAV or use DistroAV private APIs. The build workflow retrieves the NDI SDK headers from the DistroAV 6.2.1 source tree; no NDI runtime binary is bundled.

Requirements:

- OBS Studio 32.1.2 or newer
- NDI Runtime 6.3 or newer (normally already installed for DistroAV 6.2.1)
- 48 kHz OBS audio recommended

## Alpha limitations

- Windows is the only packaged target in the included workflow.
- Exact NDI source name entry is required; source discovery UI is not implemented yet.
- Sender video is converted to BGRA for the first implementation. This is reliable but uses more memory bandwidth than a native NV12 path.
- Runtime testing against real OBS + NDI hardware is still required. The source-level pairing and timeline tests pass, but this archive does not contain a precompiled DLL.
- HDR metadata and tally/PTZ/metadata forwarding are not implemented.
- Automatic fallback to silence when one selected OBS track stops producing callbacks is not implemented yet; OBS normally continues delivering silent track blocks.

## License and trademarks

Licensed under **GPL-2.0-or-later**. See `LICENSE`.

This is an independent third-party project and is not affiliated with or endorsed by the OBS Project, DistroAV, or Vizrt NDI AB. OBS and OBS Studio are registered trademarks of Wizards of OBS LLC. NDI® is a registered trademark of Vizrt NDI AB.
