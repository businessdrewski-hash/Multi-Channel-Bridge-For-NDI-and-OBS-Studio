# Troubleshooting

## Setup EXE fails or closes

Use the EXE installer rather than launching files from inside a ZIP. Close OBS and run setup as Administrator. Review:

```text
C:\ProgramData\Multichannel Bridge for DistroAV\Installer\install-script.log
C:\ProgramData\Multichannel Bridge for DistroAV\install.log
```

The portable ZIP remains available for debugging.

## The dock is missing

Search the OBS log for:

```text
[multichannel-bridge] Registered receiver proxy sources
[multichannel-bridge] Dock initialized
```

The dock is under **Docks → Multichannel Bridge for DistroAV**. DistroAV still reports its upstream base version; the bridge log markers identify the modified build.

## OBS says the old multichannel plugin is missing

Version 0.3.1 is integrated into `distroav.dll`; it does not use `ndi-multichannel-bridge.dll`. Re-run the EXE installer with stale Plugin Manager cleanup enabled. It resets `modules.json` only when the exact legacy bridge ID is present and keeps a backup.

## Two DistroAV menus appear

OBS is loading DistroAV twice. The installer disables the common duplicate locations automatically. To inspect manually:

```powershell
Get-ChildItem `
  "C:\Program Files\obs-studio", `
  "$env:ProgramData\obs-studio", `
  "$env:APPDATA\obs-studio", `
  "$env:LOCALAPPDATA\obs-studio" `
  -Filter distroav.dll -Recurse -ErrorAction SilentlyContinue |
Select-Object FullName,Length,LastWriteTime
```

There should be one active copy for the OBS installation being used.

## Gaming PC has video but no multichannel audio

1. Confirm the role is **Gaming PC / Sender**.
2. Confirm DistroAV Main Output is enabled.
3. Route desktop/game to Track A and mic to Track B.
4. Use two different tracks at 48 kHz.
5. Confirm `Paired` rises and `Discarded`/`Silence fallback` remain near zero.

A stable timestamp delta near 21.333 ms at 48 kHz is normal for the two selected OBS mixes.

## Stream PC gets video but no split audio

1. Confirm **Stream PC / Receiver** role.
2. Use one normal DistroAV NDI Source with audio enabled.
3. Select that OBS source in the bridge dock.
4. Create/repair the split sources.
5. Confirm `Detected channels: 4` and both outputs are active.

## Audio skips or jumps

- Start with NDI Frame Sync disabled on the combined receiver source.
- Remove old separate NDI desktop/mic sources.
- Check whether discarded, fallback, or missing counters increased.
- Inspect both OBS logs for reconnects, buffering changes, source rehooks, or output restarts.

## Uninstall fails

Close OBS before uninstalling. Review:

```text
C:\ProgramData\Multichannel Bridge for DistroAV\Installer\uninstall-script.log
C:\ProgramData\Multichannel Bridge for DistroAV\uninstall.log
```

The uninstaller refuses to overwrite a `distroav.dll` that changed after bridge installation unless the PowerShell helper is run manually with `-ForceRestore`.
