[CmdletBinding()]
param(
    [string]$ObsPath = "C:\Program Files\obs-studio",
    [switch]$CleanLegacyPluginManager,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$productName = 'Multichannel Bridge for DistroAV'
$version = '0.5.0-alpha1-buildfix1'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stateRoot = Join-Path $env:ProgramData $productName
$legacyStateRoot = Join-Path $env:ProgramData 'NDI-Multichannel-Bridge'
$manifestPath = Join-Path $stateRoot 'install-state.json'

New-Item -ItemType Directory -Force $stateRoot | Out-Null
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $stateRoot 'install.log'
}

function Write-Log {
    param([string]$Message)
    $line = '[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $Message
    Write-Host $line
    Add-Content -LiteralPath $LogPath -Value $line -Encoding UTF8
}

function Test-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function ConvertTo-PathKey {
    param([Parameter(Mandatory)][string]$Path)
    $bytes = [Text.Encoding]::UTF8.GetBytes($Path.ToLowerInvariant())
    return [Convert]::ToBase64String($bytes).TrimEnd('=').Replace('/', '_').Replace('+', '-')
}

function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination
    )
    New-Item -ItemType Directory -Force $Destination | Out-Null
    Copy-Item -Path (Join-Path $Source '*') -Destination $Destination -Recurse -Force
}

function Backup-DirectoryOnce {
    param(
        [Parameter(Mandatory)][string]$Source,
        [Parameter(Mandatory)][string]$Destination
    )
    if (-not (Test-Path -LiteralPath $Source)) { return $false }
    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
        Write-Log "Backed up directory: $Source"
    }
    return $true
}

try {
    if (-not (Test-Administrator)) {
        throw 'Administrator rights are required.'
    }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        throw 'OBS Studio is still running. Close OBS completely and run the installer again.'
    }

    $ObsPath = [IO.Path]::GetFullPath($ObsPath.TrimEnd('\'))
    $obsExe = Join-Path $ObsPath 'bin\64bit\obs64.exe'
    if (-not (Test-Path -LiteralPath $obsExe)) {
        throw "The selected folder does not look like an OBS Studio installation: $ObsPath"
    }

    $packageDll = Join-Path $here 'obs-plugins\64bit\distroav.dll'
    $packageData = Join-Path $here 'data\obs-plugins\distroav'
    if (-not (Test-Path -LiteralPath $packageDll)) {
        throw "Installer payload is missing: $packageDll"
    }
    if (-not (Test-Path -LiteralPath $packageData)) {
        throw "Installer payload is missing: $packageData"
    }

    $installKey = ConvertTo-PathKey -Path $ObsPath
    $backupRoot = Join-Path $stateRoot (Join-Path 'backup' $installKey)
    New-Item -ItemType Directory -Force $backupRoot | Out-Null

    # Migrate the backup created by the earlier alpha installer so upgrading does
    # not accidentally replace the user's original official DistroAV backup.
    $legacyBackup = Join-Path $legacyStateRoot 'backup'
    if ((Test-Path -LiteralPath $legacyBackup) -and
        -not (Test-Path -LiteralPath (Join-Path $backupRoot 'original-state.json'))) {
        Write-Log 'Migrating backup from the earlier alpha installer.'
        $legacyDll = Join-Path $legacyBackup 'distroav.dll'
        $legacyData = Join-Path $legacyBackup 'distroav-data'
        if (Test-Path -LiteralPath $legacyDll) {
            Copy-Item -LiteralPath $legacyDll -Destination (Join-Path $backupRoot 'distroav.dll') -Force
        }
        if (Test-Path -LiteralPath $legacyData) {
            Copy-Item -LiteralPath $legacyData -Destination (Join-Path $backupRoot 'distroav-data') -Recurse -Force
        }
        [ordered]@{
            obs_path = $ObsPath
            dll_existed = (Test-Path -LiteralPath $legacyDll)
            data_existed = (Test-Path -LiteralPath $legacyData)
            created_utc = (Get-Date).ToUniversalTime().ToString('o')
            migrated_from_legacy_installer = $true
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $backupRoot 'original-state.json') -Encoding UTF8
    }

    $installedDll = Join-Path $ObsPath 'obs-plugins\64bit\distroav.dll'
    $installedData = Join-Path $ObsPath 'data\obs-plugins\distroav'
    $originalStatePath = Join-Path $backupRoot 'original-state.json'

    # Preserve the pre-bridge DistroAV installation only once. Future upgrades
    # keep this original backup rather than backing up an older bridge build.
    if (-not (Test-Path -LiteralPath $originalStatePath)) {
        $dllExisted = Test-Path -LiteralPath $installedDll
        $dataExisted = Test-Path -LiteralPath $installedData
        if ($dllExisted) {
            Copy-Item -LiteralPath $installedDll -Destination (Join-Path $backupRoot 'distroav.dll') -Force
        }
        if ($dataExisted) {
            Copy-Item -LiteralPath $installedData -Destination (Join-Path $backupRoot 'distroav-data') -Recurse -Force
        }
        [ordered]@{
            obs_path = $ObsPath
            dll_existed = $dllExisted
            data_existed = $dataExisted
            created_utc = (Get-Date).ToUniversalTime().ToString('o')
            migrated_from_legacy_installer = $false
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $originalStatePath -Encoding UTF8
        Write-Log 'Created one-time backup of the pre-bridge DistroAV installation.'
    }

    $previousState = $null
    if (Test-Path -LiteralPath $manifestPath) {
        try { $previousState = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json } catch { $previousState = $null }
    }

    $disabledDuplicates = New-Object System.Collections.Generic.List[object]
    if ($previousState -and $previousState.disabled_duplicate_installations) {
        foreach ($item in $previousState.disabled_duplicate_installations) {
            $disabledDuplicates.Add([pscustomobject]@{
                original_path = [string]$item.original_path
                backup_path = [string]$item.backup_path
            })
        }
    }

    # OBS can load DistroAV from ProgramData or the current user's plugin folder
    # in addition to Program Files. Disable those exact duplicate locations so
    # the custom DLL is initialized only once.
    $duplicateDirs = @(
        (Join-Path $env:ProgramData 'obs-studio\plugins\distroav'),
        (Join-Path $env:APPDATA 'obs-studio\plugins\distroav'),
        (Join-Path $env:LOCALAPPDATA 'obs-studio\plugins\distroav')
    ) | Select-Object -Unique

    foreach ($duplicateDir in $duplicateDirs) {
        if (-not (Test-Path -LiteralPath $duplicateDir)) { continue }
        $alreadyTracked = $false
        foreach ($tracked in $disabledDuplicates) {
            if ([string]::Equals($tracked.original_path, $duplicateDir, [StringComparison]::OrdinalIgnoreCase)) {
                $alreadyTracked = $true
                break
            }
        }
        $key = ConvertTo-PathKey -Path $duplicateDir
        $backupPath = Join-Path $backupRoot (Join-Path 'disabled-duplicates' $key)
        if (-not $alreadyTracked) {
            Backup-DirectoryOnce -Source $duplicateDir -Destination $backupPath | Out-Null
            $disabledDuplicates.Add([pscustomobject]@{
                original_path = $duplicateDir
                backup_path = $backupPath
            })
        }
        Remove-Item -LiteralPath $duplicateDir -Recurse -Force
        Write-Log "Disabled duplicate DistroAV installation: $duplicateDir"
    }

    # Remove the obsolete standalone v0.2 module. Version 0.3+ is integrated
    # into distroav.dll and must never be loaded beside it.
    $legacyTargets = @(
        (Join-Path $ObsPath 'obs-plugins\64bit\ndi-multichannel-bridge.dll'),
        (Join-Path $ObsPath 'data\obs-plugins\ndi-multichannel-bridge'),
        (Join-Path $env:ProgramData 'obs-studio\plugins\ndi-multichannel-bridge'),
        (Join-Path $env:APPDATA 'obs-studio\plugins\ndi-multichannel-bridge'),
        (Join-Path $env:LOCALAPPDATA 'obs-studio\plugins\ndi-multichannel-bridge')
    ) | Select-Object -Unique
    foreach ($target in $legacyTargets) {
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force
            Write-Log "Removed obsolete standalone bridge component: $target"
        }
    }

    # OBS 32's Plugin Manager can retain a stale record for the removed v0.2
    # module. Reset its registry only when that exact legacy ID is present.
    $pluginManagerReset = $false
    if ($CleanLegacyPluginManager) {
        $modulesJson = Join-Path $env:APPDATA 'obs-studio\plugin_manager\modules.json'
        if (Test-Path -LiteralPath $modulesJson) {
            $raw = Get-Content -LiteralPath $modulesJson -Raw -ErrorAction Stop
            if ($raw -match '(?i)ndi[_-]multichannel[_-]bridge') {
                $backupName = 'modules.json.before-mcb-{0}' -f (Get-Date -Format 'yyyyMMdd-HHmmss')
                $backupPath = Join-Path $backupRoot $backupName
                Copy-Item -LiteralPath $modulesJson -Destination $backupPath -Force
                Remove-Item -LiteralPath $modulesJson -Force
                $pluginManagerReset = $true
                Write-Log 'Removed stale standalone bridge entry by resetting OBS Plugin Manager state. A backup was retained.'
            }
        }
    }

    foreach ($folder in @('obs-plugins', 'data')) {
        $source = Join-Path $here $folder
        $destination = Join-Path $ObsPath $folder
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Installer payload folder is missing: $source"
        }
        New-Item -ItemType Directory -Force $destination | Out-Null
        Copy-Item -Path (Join-Path $source '*') -Destination $destination -Recurse -Force
    }

    $packageHash = (Get-FileHash -LiteralPath $packageDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $installedHash = (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($packageHash -ne $installedHash) {
        throw 'The installed distroav.dll hash does not match the installer payload.'
    }

    $activeDlls = New-Object System.Collections.Generic.List[string]
    $candidateDlls = @(
        $installedDll,
        (Join-Path $env:ProgramData 'obs-studio\plugins\distroav\bin\64bit\distroav.dll'),
        (Join-Path $env:APPDATA 'obs-studio\plugins\distroav\bin\64bit\distroav.dll'),
        (Join-Path $env:LOCALAPPDATA 'obs-studio\plugins\distroav\bin\64bit\distroav.dll')
    ) | Select-Object -Unique
    foreach ($candidate in $candidateDlls) {
        if (Test-Path -LiteralPath $candidate) { $activeDlls.Add($candidate) }
    }
    if ($activeDlls.Count -ne 1) {
        throw "Expected exactly one active distroav.dll after installation, found $($activeDlls.Count): $($activeDlls -join '; ')"
    }

    $state = [ordered]@{
        product = $productName
        version = $version
        installed_utc = (Get-Date).ToUniversalTime().ToString('o')
        obs_path = $ObsPath
        backup_root = $backupRoot
        installed_dll = $installedDll
        installed_dll_sha256 = $installedHash
        plugin_manager_reset = $pluginManagerReset
        disabled_duplicate_installations = @($disabledDuplicates)
    }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    Write-Log "$productName $version installed successfully to $ObsPath"
    Write-Log 'Install the same setup executable on both PCs, then choose the role in Docks > Multichannel Bridge for DistroAV.'
    exit 0
} catch {
    Write-Log ("INSTALL FAILED: " + $_.Exception.Message)
    Write-Error $_
    exit 1
}
