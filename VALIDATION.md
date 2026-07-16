# Validation notes for v0.3.1-alpha

Source-side checks:

- `patch_distroav.py` passes Python bytecode compilation.
- The patch rules retain structural verification for the sender, receiver hook, dock, and DistroAV build list.
- Product-facing version and dock strings were updated without changing saved configuration keys or proxy source IDs.
- The EXE installer validates the OBS root, blocks installation while OBS runs, invokes an elevated PowerShell helper, records logs, and provides an uninstall path.
- The helper backs up the original DistroAV install, removes obsolete v0.2 files, disables duplicate plugin locations, copies the payload, verifies the DLL hash, and confirms a single active common-path DLL.
- The release workflow requires both the EXE and portable ZIP before producing checksums.

Authoritative tests still required from GitHub Actions and Windows:

- Real DistroAV/OBS compilation and linking.
- Inno Setup script compilation.
- Fresh install, upgrade, repair, and uninstall on Windows.
- OBS load with exactly one DistroAV menu and the renamed bridge dock.
- Long-duration A/V sync, reconnect, Frame Sync on/off, recording, and stream tests.

Treat this as controlled-test alpha software until those tests pass.
