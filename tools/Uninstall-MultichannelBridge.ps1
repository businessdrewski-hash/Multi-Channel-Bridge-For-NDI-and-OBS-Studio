[CmdletBinding()]
param(
    [string]$ObsPath,
    [switch]$ForceRestore,
    [switch]$RestoreDuplicateInstallations,
    [string]$LogPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$productName = 'Multichannel Bridge for DistroAV'
$stateRoot = Join-Path $env:ProgramData $productName
$manifestPath = Join-Path $stateRoot 'install-state.json'
if ([string]::IsNullOrWhiteSpace($LogPath)) {
    $LogPath = Join-Path $stateRoot 'uninstall.log'
}
New-Item -ItemType Directory -Force $stateRoot | Out-Null

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

try {
    if (-not (Test-Administrator)) { throw 'Administrator rights are required.' }
    if (Get-Process obs64 -ErrorAction SilentlyContinue) {
        throw 'OBS Studio is still running. Close OBS completely before uninstalling.'
    }
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Installation state was not found: $manifestPath"
    }

    $state = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([string]::IsNullOrWhiteSpace($ObsPath)) { $ObsPath = [string]$state.obs_path }
    $ObsPath = [IO.Path]::GetFullPath($ObsPath.TrimEnd('\'))
    $backupRoot = [string]$state.backup_root

    $installedDll = Join-Path $ObsPath 'obs-plugins\64bit\distroav.dll'
    $installedData = Join-Path $ObsPath 'data\obs-plugins\distroav'
    $expectedHash = [string]$state.installed_dll_sha256

    if (Test-Path -LiteralPath $installedDll) {
        $currentHash = (Get-FileHash -LiteralPath $installedDll -Algorithm SHA256).Hash.ToLowerInvariant()
        if (-not $ForceRestore -and $expectedHash -and $currentHash -ne $expectedHash) {
            throw 'The installed distroav.dll has changed since this bridge was installed. Re-run the uninstaller with -ForceRestore only if overwriting it is intentional.'
        }
        Remove-Item -LiteralPath $installedDll -Force
    }
    if (Test-Path -LiteralPath $installedData) {
        Remove-Item -LiteralPath $installedData -Recurse -Force
    }

    $originalStatePath = Join-Path $backupRoot 'original-state.json'
    if (Test-Path -LiteralPath $originalStatePath) {
        $original = Get-Content -LiteralPath $originalStatePath -Raw | ConvertFrom-Json
        if ([bool]$original.dll_existed) {
            $backupDll = Join-Path $backupRoot 'distroav.dll'
            if (-not (Test-Path -LiteralPath $backupDll)) { throw 'Original DistroAV DLL backup is missing.' }
            New-Item -ItemType Directory -Force (Split-Path -Parent $installedDll) | Out-Null
            Copy-Item -LiteralPath $backupDll -Destination $installedDll -Force
        }
        if ([bool]$original.data_existed) {
            $backupData = Join-Path $backupRoot 'distroav-data'
            if (-not (Test-Path -LiteralPath $backupData)) { throw 'Original DistroAV data backup is missing.' }
            New-Item -ItemType Directory -Force (Split-Path -Parent $installedData) | Out-Null
            Copy-Item -LiteralPath $backupData -Destination $installedData -Recurse -Force
        }
        Write-Log 'Removed the bridge build and restored the pre-bridge DistroAV installation.'
    } else {
        Write-Log 'Removed the bridge build. No pre-bridge DistroAV backup was available.'
    }

    if ($RestoreDuplicateInstallations -and $state.disabled_duplicate_installations) {
        foreach ($item in $state.disabled_duplicate_installations) {
            $originalPath = [string]$item.original_path
            $backupPath = [string]$item.backup_path
            if ((Test-Path -LiteralPath $backupPath) -and -not (Test-Path -LiteralPath $originalPath)) {
                New-Item -ItemType Directory -Force (Split-Path -Parent $originalPath) | Out-Null
                Copy-Item -LiteralPath $backupPath -Destination $originalPath -Recurse -Force
                Write-Log "Restored previously disabled duplicate installation: $originalPath"
            }
        }
    } elseif ($state.disabled_duplicate_installations) {
        Write-Log "Previously disabled duplicate DistroAV copies were not restored, to avoid recreating duplicate plugin loading. Backups remain under: $backupRoot"
    }

    Remove-Item -LiteralPath $manifestPath -Force -ErrorAction SilentlyContinue
    Write-Log "$productName uninstalled successfully."
    exit 0
} catch {
    Write-Log ("UNINSTALL FAILED: " + $_.Exception.Message)
    Write-Error $_
    exit 1
}
