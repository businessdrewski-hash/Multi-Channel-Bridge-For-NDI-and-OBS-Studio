$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Convert-TestState {
    param([System.Collections.Generic.List[object]]$Items)

    $state = [ordered]@{
        disabled_duplicate_installations = $Items.ToArray()
    }
    return $state | ConvertTo-Json -Depth 8 | ConvertFrom-Json
}

$empty = New-Object System.Collections.Generic.List[object]
$emptyState = Convert-TestState -Items $empty
if ($null -eq $emptyState.disabled_duplicate_installations -or
    @($emptyState.disabled_duplicate_installations).Count -ne 0) {
    throw 'Empty duplicate-installation state did not round-trip as an empty array.'
}

$populated = New-Object System.Collections.Generic.List[object]
$populated.Add([pscustomobject]@{
    original_path = 'C:\ProgramData\obs-studio\plugins\distroav'
    backup_path = 'C:\ProgramData\Multichannel Bridge for DistroAV\backup\duplicate'
})
$populatedState = Convert-TestState -Items $populated
if (@($populatedState.disabled_duplicate_installations).Count -ne 1 -or
    $populatedState.disabled_duplicate_installations.original_path -notlike 'C:\ProgramData\*') {
    throw 'Populated duplicate-installation state did not round-trip correctly.'
}

Write-Host 'Windows PowerShell installer-state serialization test passed'
