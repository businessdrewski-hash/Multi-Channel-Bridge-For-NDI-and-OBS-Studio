# Third-party notices

## OBS Studio

This plugin links to OBS Studio/libobs and is distributed under GPL-2.0-or-later. OBS Studio source and license notices are available from the OBS Project.

## DistroAV

The Windows build workflow downloads the DistroAV 6.2.1 source archive only to copy its `lib/ndi` SDK header folder for compilation. DistroAV is GPL-2.0 licensed. No DistroAV binary or source file is included in the distributed plugin package.

## NDI SDK headers and runtime

The NDI SDK headers carry their own Vizrt NDI AB notices and license terms. The build process must preserve those headers' notices. The plugin dynamically loads an independently installed NDI Runtime and does not bundle the NDI runtime library.
