# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory = $true)][string] $Program,
    [Parameter(Mandatory = $true)][string] $FaultProgram,
    [Parameter(Mandatory = $true)][string] $Fixture,
    [Parameter(Mandatory = $true)][string] $ExpectedHash,
    [string] $FirstCaseTrace = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-volume-common.ps1"

# Drive letters and volume handles can change after SET_DRIVE_LAYOUT_EX.
function Find-TestPartition {
    param([string] $Image, [uint64] $Offset)
    Write-Host "Discovering partition at offset $Offset in $Image"
    for ($Attempt = 0; $Attempt -lt 50; ++$Attempt) {
        # Mount/re-attach and the CLI's IOCTL_DISK_UPDATE_PROPERTIES already
        # refresh this disk. Do not rescan all host disks on every polling attempt.
        $Disk = Get-DiskImage -ImagePath $Image | Get-Disk
        $Matches = @(Get-Partition -DiskNumber $Disk.Number |
            Where-Object { $_.Offset -eq $Offset })
        if ($Matches.Count -eq 1) {
            $Partition = $Matches[0]
            if (-not $Partition.DriveLetter) {
                $Partition | Add-PartitionAccessPath -AssignDriveLetter
                $Partition = Get-Partition -DiskNumber $Disk.Number `
                    -PartitionNumber $Partition.PartitionNumber
            }
            return $Partition
        }
        Start-Sleep -Milliseconds 100
    }
    throw 'Could not rediscover the test partition by its disk and offset'
}

function Get-TestTarget {
    param($Partition, [bool] $UseGuid)
    if ($UseGuid) {
        return ((& mountvol.exe "$($Partition.DriveLetter):" /L) |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 1).Trim()
    }
    return "$($Partition.DriveLetter):"
}

function Invoke-TestCommand {
    param([string] $Exe, [string[]] $Arguments, [int] $ExitCode, [string] $Text)
    Write-Host "Running $Exe $Arguments"
    $Output = & $Exe @Arguments 2>&1
    $Actual = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Actual -eq $ExitCode) "Expected exit $ExitCode, got $Actual"
    Assert-Condition (($Output -join "`n").Contains($Text)) "Missing output: $Text"
}

function Test-ShrinkPartition {
    param([string] $Case, [bool] $UseGuid, [bool] $PartitionOnly,
        [string] $Fault = '', [int] $ExitCode = 0, [string] $Text = 'shrank the partition')
    $Image = Join-Path $Temporary "$Case.vhdx"
    Copy-Item -LiteralPath $Fixture -Destination $Image -Force
    $Mounted = $false
    try {
        Write-Host "windows-shrink-partition ($Case): attaching $Image"
        $DiskImage = Mount-DiskImage -ImagePath $Image -StorageType VHDX -PassThru
        $Mounted = $true
        $Disk = $DiskImage | Get-Disk
        $DiskId = $Disk.UniqueId
        $Initial = @(Get-Partition -DiskNumber $Disk.Number |
            Where-Object { $_.Type -ne 'Reserved' })[0]
        $Partition = Find-TestPartition $Image $Initial.Offset
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        $Target = Get-TestTarget $Partition $UseGuid
        $Baseline = @(Get-FixtureManifest "$($Partition.DriveLetter):\")
        Assert-InitialFixture $Baseline $PayloadHash
        $Size = if ($PartitionOnly) { [uint64] 64MB } else { [uint64] 48MB }

        if ($Fault -eq '') {
            # All reject paths run before any filesystem or partition write.
            foreach ($BadSize in @([uint64] 80MB, [uint64] 96MB, [uint64] 1MB)) {
                $Output = & $Program --shrink-partition $Target $BadSize 2>&1
                $Result = $LASTEXITCODE
                $Output | ForEach-Object { Write-Host $_ }
                Assert-Condition ($Result -eq 1) "Accepted invalid target $BadSize"
                Assert-Condition (($Output -join "`n") -match 'no filesystem write was attempted') `
                    'Preflight rejection did not report no writes'
                $Partition = Find-TestPartition $Image $Initial.Offset
                Assert-Condition ($Partition.Size -eq 96MB) 'Rejected shrink changed partition'
                $Volume = Wait-Volume ([char] $Partition.DriveLetter)
                Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
                    'Rejected shrink changed data or metadata'
            }
        }
        $env:EXFAT_RESIZE_TEST_PARTITION_FAULT = $Fault
        $Exe = if ($Fault -eq '') { $Program } else { $FaultProgram }
        # Exercise filesystem-sector rounding as well as the exact-size retry path.
        $Requested = $Size + 1
        Invoke-TestCommand $Exe @('--shrink-partition', $Target, [string] $Requested) `
            $ExitCode $Text
        Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue

        # Reattach to inspect persisted partition geometry, including after injected errors.
        Write-Host "windows-shrink-partition ($Case): detaching for persistence check"
        Dismount-TestDiskImage $Image
        $Mounted = $false
        Write-Host "windows-shrink-partition ($Case): reattaching for persistence check"
        Mount-DiskImage -ImagePath $Image -StorageType VHDX | Out-Null
        $Mounted = $true
        $Partition = Find-TestPartition $Image $Initial.Offset
        Write-Host "windows-shrink-partition ($Case): verifying persisted layout"
        $Disk = Get-DiskImage -ImagePath $Image | Get-Disk
        Assert-Condition ($Disk.UniqueId -eq $DiskId) 'Disk identity changed'
        Assert-Condition ($Partition.Offset -eq $Initial.Offset) 'Partition start changed'
        Assert-Condition ($Partition.Guid -eq $Initial.Guid) 'Partition identity changed'
        Assert-Condition ($Partition.GptType -eq $Initial.GptType) 'GPT type changed'
        Assert-Condition ($Partition.MbrType -eq $Initial.MbrType) 'MBR type changed'
        $NotPublished = $Fault -in @('cancel-discovery', 'shrink-revalidate',
            'shrink-before-set', 'shrink-layout-changed', 'dismount')
        $ExpectedSize = if ($NotPublished) { [uint64] 96MB } else { $Size }
        Assert-Condition ($Partition.Size -eq $ExpectedSize) 'Unexpected persisted partition size'
        Write-Host "windows-shrink-partition ($Case): waiting for volume and checking contents"
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after $Case"
        $Target = Get-TestTarget $Partition $UseGuid
        if ($NotPublished) {
            # Cancellation leaves the original filesystem; other failures leave a
            # clean smaller filesystem. The same option handles both cases.
            Invoke-TestCommand $Program @('--shrink-partition', $Target, [string] $Size) `
                0 'shrank the partition'
            $Partition = Find-TestPartition $Image $Initial.Offset
            $Volume = Wait-Volume ([char] $Partition.DriveLetter)
            $Target = Get-TestTarget $Partition $UseGuid
        }
        Write-Host "windows-shrink-partition ($Case): checking the smaller filesystem"
        Invoke-CleanCheck ([char] $Partition.DriveLetter)
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after CHKDSK for $Case"
        # Ensure the shared discovery refactor and the new end still allow growth.
        Write-Host "windows-shrink-partition ($Case): growing back to 160 MiB"
        Invoke-TestCommand $Program @('--grow-partition', $Target, [string] ([uint64] 160MB)) `
            0 'grew the partition'
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after regrowth for $Case"
        Invoke-CleanCheck ([char] $Partition.DriveLetter)
    }
    finally {
        Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue
        if ($Mounted) {
            Write-Host "windows-shrink-partition ($Case): detaching during cleanup"
            Dismount-TestDiskImage $Image
        }
    }
    Write-Host "windows-shrink-partition ($Case): passed"
}

$Program = (Resolve-Path -LiteralPath $Program).Path
$FaultProgram = (Resolve-Path -LiteralPath $FaultProgram).Path
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
$PayloadHash = (Get-Content -LiteralPath $ExpectedHash -Raw).Trim().ToUpperInvariant()
$Temporary = Join-Path $env:RUNNER_TEMP "exfat-resize-shrink-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $Temporary | Out-Null
# Capture the first complete case, including device creation and both detach
# calls. Later cases run without tracing so the recording stays small.
if ($FirstCaseTrace) {
    $FirstCaseTrace = [System.IO.Path]::GetFullPath($FirstCaseTrace)
    New-Item -ItemType Directory -Path (Split-Path $FirstCaseTrace) -Force | Out-Null
    $TraceSession = "exfat-resize-$([guid]::NewGuid())"
    Write-Host "Recording first shrink case to $FirstCaseTrace"
    & wpr.exe -start GeneralProfile -filemode -instancename $TraceSession
    Assert-Condition ($LASTEXITCODE -eq 0) 'Could not start Windows Performance Recorder'
}
try {
    Test-ShrinkPartition 'combined-drive' $false $false
}
finally {
    if ($FirstCaseTrace) {
        & wpr.exe -stop $FirstCaseTrace -compress -instancename $TraceSession
        $TraceSaved = $LASTEXITCODE -eq 0
        if (-not $TraceSaved) {
            # Only cancel our own recording, and preserve any test failure.
            Write-Warning 'Could not save the Windows performance trace'
            & wpr.exe -cancel -instancename $TraceSession
        }
    }
}
if ($FirstCaseTrace) {
    Assert-Condition ($TraceSaved -and (Test-Path -LiteralPath $FirstCaseTrace)) `
        'Performance trace was not saved'
}
Test-ShrinkPartition 'combined-guid' $true $false
Test-ShrinkPartition 'partition-only' $false $true
Test-ShrinkPartition 'cancel-before-writes' $false $false 'cancel-discovery' 130 'interrupted by user'
Test-ShrinkPartition 'cancel-during-commit' $false $false 'cancel-shrink'
Test-ShrinkPartition 'changed-layout' $false $false 'shrink-layout-changed' 1 'layout changed'
Test-ShrinkPartition 'revalidation-io' $false $false 'shrink-revalidate' 1 'partition was not changed'
Test-ShrinkPartition 'dismount-failure' $false $false 'dismount' 1 'partition was not changed'
foreach ($Fault in @('shrink-before-set', 'shrink-result', 'shrink-flush',
    'shrink-refresh', 'shrink-readback')) {
    Test-ShrinkPartition $Fault $false $false $Fault 1 'its result is uncertain'
}
Test-ShrinkPartition 'volume-unavailable' $false $false 'shrink-volume' 1 `
    'the smaller partition and filesystem are synchronized'
Write-Host 'windows-shrink-partition: passed'
