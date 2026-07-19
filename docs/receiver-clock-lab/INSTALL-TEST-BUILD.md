# Installing the test build

Receiver Clock Lab uses DistroAV's existing module and source IDs so that it exercises the real DistroAV receiver path.
It therefore replaces stock DistroAV for the test; the two builds cannot be loaded together.

1. Close OBS.
2. Back up the current DistroAV installer or portable plugin folder.
3. Install/extract the Receiver Clock Lab artifact using the same method as DistroAV.
4. Start OBS and confirm the log reports `DistroAV Receiver Clock Lab` version `6.2.1.1`.
5. Open the existing NDI Source, choose a receiver-clock comparison mode, and enable diagnostics.

The test does not require an Audio Clock, Sync Guardian, or Multichannel Bridge filter. Use a normal DistroAV NDI Source
with audio enabled. Restore stock DistroAV after the experiment by closing OBS and reinstalling the stock package.

The live diagnostic file path is printed in the OBS log. The source-properties export button creates a second snapshot in
the same plugin configuration directory.
