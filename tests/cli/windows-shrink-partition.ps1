# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory = $true)][string] $Program,
    [Parameter(Mandatory = $true)][string] $FaultProgram,
    [Parameter(Mandatory = $true)][string] $Fixture,
    [Parameter(Mandatory = $true)][string] $TailFixture,
    [Parameter(Mandatory = $true)][string] $ExpectedHash,
    [System.Collections.Generic.List[object]] $DetachJobs = $null
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/windows-volume-common.ps1"

# Drive letters and volume handles can change after SET_DRIVE_LAYOUT_EX.
function Find-TestPartition {
    param([string] $Image, [uint64] $Offset)
    for ($Attempt = 0; $Attempt -lt 50; ++$Attempt) {
        # Attachment and the CLI's IOCTL_DISK_UPDATE_PROPERTIES already
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
    param([string] $Exe, [string[]] $Arguments, [int] $ExitCode, [string] $Text,
        [string] $AdditionalText = '')
    $Output = & $Exe @Arguments 2>&1
    $Actual = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Actual -eq $ExitCode) `
        "Expected exit $ExitCode, got $Actual from $Exe $Arguments"
    Assert-Condition (($Output -join "`n").Contains($Text)) `
        "Missing output '$Text' from $Exe $Arguments"
    if ($AdditionalText) {
        Assert-Condition (($Output -join "`n").Contains($AdditionalText)) `
            "Missing output '$AdditionalText' from $Exe $Arguments"
    }
}

# Read boot copies and the fixture's retained bitmap directly through the disk,
# independently of the CLI's stage report and the mounted driver's size cache.
function Get-ShrinkState {
    param($Disk, $Partition)
    $Stream = [System.IO.FileStream]::new("\\.\PhysicalDrive$($Disk.Number)",
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        function Read-Sector {
            param([uint64] $Sector)
            $null = $Stream.Seek([int64] ($Partition.Offset + $Sector * 512),
                [System.IO.SeekOrigin]::Begin)
            $Bytes = [byte[]]::new(512)
            Assert-Condition ($Stream.Read($Bytes, 0, $Bytes.Length) -eq $Bytes.Length) `
                'Short raw fixture read'
            return ,$Bytes
        }
        $Main = Read-Sector 0
        $Backup = Read-Sector 12
        foreach ($Boot in @($Main, $Backup)) {
            Assert-Condition ([System.Text.Encoding]::ASCII.GetString($Boot, 3, 8) -eq 'EXFAT   ') `
                'Missing exFAT boot signature'
            Assert-Condition ($Boot[108] -eq 9) 'Fixture is not using 512-byte sectors'
        }
        $Geometry = [Convert]::ToBase64String([byte[]] $Main[64..105])
        Assert-Condition ($Geometry -eq [Convert]::ToBase64String([byte[]] $Backup[64..105])) `
            'Main and backup geometry differ'
        Assert-Condition ($Main[109] -eq $Backup[109]) 'Main and backup cluster sizes differ'
        $Length = [BitConverter]::ToUInt64($Main, 72)
        $Heap = [BitConverter]::ToUInt32($Main, 88)
        $Count = [BitConverter]::ToUInt32($Main, 92)
        $SPC = [uint32] [Math]::Pow(2, $Main[109])
        Assert-Condition ($Count -eq [Math]::Floor(($Length - $Heap) / $SPC)) `
            'Filesystem length and heap geometry differ'
        Assert-Condition ($Count -le 4096) 'Fixture bitmap no longer fits one sector'
        $Root = [BitConverter]::ToUInt32($Main, 96)
        $Entries = Read-Sector ($Heap + ($Root - 2) * $SPC)
        $BitmapCluster = [uint32] 0
        for ($Index = 0; $Index -lt 512; $Index += 32) {
            if ($Entries[$Index] -eq 0x81) {
                $BitmapCluster = [BitConverter]::ToUInt32($Entries, $Index + 20)
                break
            }
        }
        Assert-Condition ($BitmapCluster -ge 2) 'Missing fixture bitmap'
        $Bitmap = Read-Sector ($Heap + ($BitmapCluster - 2) * $SPC)
        $FirstTailBit = [uint32] [Math]::Floor((48MB / 512 - $Heap) / $SPC)
        $TailCount = 0
        for ($Bit = $FirstTailBit; $Bit -lt $Count; ++$Bit) {
            if ($Bitmap[[int] [Math]::Floor($Bit / 8)] -band (1 -shl ($Bit % 8))) {
                ++$TailCount
            }
        }
        return [pscustomobject] @{
            Geometry = $Geometry
            Length = $Length * 512
            MainFlags = [BitConverter]::ToUInt16($Main, 106)
            BackupFlags = [BitConverter]::ToUInt16($Backup, 106)
            TailCount = $TailCount
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function Assert-CleanShrinkState {
    param($State, [uint64] $Length)
    Assert-Condition ($State.Length -eq $Length) 'Unexpected filesystem length'
    Assert-Condition (($State.MainFlags -band 6) -eq 0 -and ($State.BackupFlags -band 6) -eq 0) `
        'Filesystem is dirty or reports media failure'
}

function Test-ShrinkPartition {
    param([string] $Case, [bool] $UseGuid, [bool] $PartitionOnly,
        [string] $Fault = '', [int] $ExitCode = 0, [string] $Text = 'shrank the partition',
        [bool] $Compaction = $false)
    $Image = Join-Path $Temporary "$Case.vhdx"
    $SourceFixture = if ($Compaction) { $TailFixture } else { $Fixture }
    Copy-Item -LiteralPath $SourceFixture -Destination $Image -Force
    $Mounted = $false
    try {
        Write-Host "windows-shrink-partition ($Case): starting"
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
        $CompactionCancellation = $Fault -in @('cancel-compaction-early', 'cancel-compaction-late')
        $ExpectedMoves = if ($Fault -eq 'cancel-compaction-early') { 1 } else { 3 }
        $AdditionalText = ''
        if ($Compaction) {
            $SourceState = Get-ShrinkState $Disk $Partition
            Assert-Condition ($SourceState.Length -eq 64MB -and $SourceState.TailCount -eq 4) `
                'Compaction fixture does not require the four expected tail moves'
            if ($CompactionCancellation) {
                $AdditionalText = "SOURCE_READY after $ExpectedMoves of 4 synchronized tail moves"
            }
        }

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
                Assert-ManifestEqual $Baseline `
                    @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
                    'Rejected shrink changed data or metadata'
            }
        }
        $env:EXFAT_RESIZE_TEST_PARTITION_FAULT = $Fault
        $Exe = if ($Fault -eq '') { $Program } else { $FaultProgram }
        # Exercise filesystem-sector rounding as well as the exact-size retry path.
        $Requested = $Size + 1
        Invoke-TestCommand $Exe @('--shrink-partition', $Target, [string] $Requested) `
            $ExitCode $Text $AdditionalText
        Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue

        # Refresh and inspect the updated layout while the disk remains attached.
        Update-HostStorageCache
        $Partition = Find-TestPartition $Image $Initial.Offset
        $Disk = Get-DiskImage -ImagePath $Image | Get-Disk
        Assert-Condition ($Disk.UniqueId -eq $DiskId) 'Disk identity changed'
        Assert-Condition ($Partition.Offset -eq $Initial.Offset) 'Partition start changed'
        Assert-Condition ($Partition.Guid -eq $Initial.Guid) 'Partition identity changed'
        Assert-Condition ($Partition.GptType -eq $Initial.GptType) 'GPT type changed'
        Assert-Condition ($Partition.MbrType -eq $Initial.MbrType) 'MBR type changed'
        $NotPublished = $Fault -in @('cancel-discovery', 'shrink-revalidate',
            'shrink-before-set', 'shrink-layout-changed', 'dismount') -or $CompactionCancellation
        $ExpectedSize = if ($NotPublished) { [uint64] 96MB } else { $Size }
        Assert-Condition ($Partition.Size -eq $ExpectedSize) 'Unexpected partition size'
        if ($Compaction) {
            $State = Get-ShrinkState $Disk $Partition
            $ExpectedLength = if ($CompactionCancellation) { [uint64] 64MB } else { $Size }
            Assert-CleanShrinkState $State $ExpectedLength
            $ExpectedTail = if ($CompactionCancellation) { 4 - $ExpectedMoves } else { 0 }
            Assert-Condition ($State.TailCount -eq $ExpectedTail) 'Unexpected remaining tail allocations'
            if ($CompactionCancellation) {
                Assert-Condition ($State.Geometry -eq $SourceState.Geometry) `
                    'SOURCE_READY changed the original geometry'
            }
        }
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after $Case"
        if ($CompactionCancellation) {
            # Verify clean cancellation through a fresh native mount before any
            # repair-capable checker or retry can mask a cache/cleanup defect.
            Dismount-DiskImage -ImagePath $Image -StorageType VHDX
            $Mounted = $false
            $DiskImage = Mount-DiskImage -ImagePath $Image -StorageType VHDX -PassThru
            $Mounted = $true
            $Disk = $DiskImage | Get-Disk
            $Partition = Find-TestPartition $Image $Initial.Offset
            Assert-Condition ($Disk.UniqueId -eq $DiskId -and $Partition.Size -eq 96MB) `
                'Cancelled partition changed after remount'
            $Volume = Wait-Volume ([char] $Partition.DriveLetter)
            $State = Get-ShrinkState $Disk $Partition
            Assert-CleanShrinkState $State ([uint64] 64MB)
            Assert-Condition ($State.Geometry -eq $SourceState.Geometry -and
                $State.TailCount -eq $ExpectedTail) 'Compaction state changed after remount'
            Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
                "Cancelled fixture after remount for $Case"
            & chkdsk.exe "$($Partition.DriveLetter):"
            Assert-Condition ($LASTEXITCODE -eq 0) `
                'Read-only CHKDSK rejected the cancelled original-size filesystem'
            $Volume = Wait-Volume ([char] $Partition.DriveLetter)
            Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
                "Cancelled fixture after CHKDSK for $Case"
        }
        $Target = Get-TestTarget $Partition $UseGuid
        if ($NotPublished) {
            # Cancellation leaves the original filesystem; other failures leave a
            # clean smaller filesystem. The same option handles both cases.
            Invoke-TestCommand $Program @('--shrink-partition', $Target, [string] $Size) `
                0 'shrank the partition'
            Update-HostStorageCache
            $Partition = Find-TestPartition $Image $Initial.Offset
            Assert-Condition ($Partition.Size -eq $Size) 'Retry did not shrink the partition'
            $Volume = Wait-Volume ([char] $Partition.DriveLetter)
            $Target = Get-TestTarget $Partition $UseGuid
            if ($Compaction) {
                Assert-CleanShrinkState (Get-ShrinkState $Disk $Partition) $Size
                Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
                    "Fixture after fresh retry for $Case"
            }
        }
        Invoke-CleanCheck ([char] $Partition.DriveLetter)
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after CHKDSK for $Case"
        # Verify the smaller partition and filesystem can be grown again.
        Invoke-TestCommand $Program @('--grow-partition', $Target, [string] ([uint64] 160MB)) `
            0 'grew the partition'
        Update-HostStorageCache
        $Partition = Find-TestPartition $Image $Initial.Offset
        Assert-Condition ($Partition.Size -eq 160MB) 'Regrowth did not enlarge the partition'
        $Volume = Wait-Volume ([char] $Partition.DriveLetter)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest "$($Partition.DriveLetter):\") `
            "Fixture after regrowth for $Case"
        Invoke-CleanCheck ([char] $Partition.DriveLetter)
    }
    finally {
        Set-Location $env:GITHUB_WORKSPACE
        Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue
        if ($Mounted) {
            Start-TestDiskImageDetach $Image $DetachJobs
        }
    }
    Write-Host "windows-shrink-partition ($Case): checks passed; detach cleanup queued"
}

$Program = (Resolve-Path -LiteralPath $Program).Path
$FaultProgram = (Resolve-Path -LiteralPath $FaultProgram).Path
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
$TailFixture = (Resolve-Path -LiteralPath $TailFixture).Path
$PayloadHash = (Get-Content -LiteralPath $ExpectedHash -Raw).Trim().ToUpperInvariant()
Write-Host "windows-shrink-partition: testing $(Split-Path -Leaf $Fixture)"
$Temporary = Join-Path $env:RUNNER_TEMP "exfat-resize-shrink-$([guid]::NewGuid())"
New-Item -ItemType Directory -Path $Temporary | Out-Null
$OwnDetachJobs = $null -eq $DetachJobs
if ($OwnDetachJobs) {
    $DetachJobs = [System.Collections.Generic.List[object]]::new()
}
# CI shares this queue between GPT and MBR. Standalone runs wait before returning.
try {
    Test-ShrinkPartition 'combined-drive' $false $false
    Test-ShrinkPartition 'combined-guid' $true $false
    Test-ShrinkPartition 'partition-only' $false $true
    Test-ShrinkPartition 'tail-compaction' $false $false -Compaction $true
    Test-ShrinkPartition 'cancel-compaction-early' $false $false 'cancel-compaction-early' 130 `
        'the original-size filesystem is clean' -Compaction $true
    Test-ShrinkPartition 'cancel-compaction-late' $false $false 'cancel-compaction-late' 130 `
        'the original-size filesystem is clean' -Compaction $true
    Test-ShrinkPartition 'cancel-before-writes' $false $false 'cancel-discovery' 130 `
        'interrupted by user'
    Test-ShrinkPartition 'cancel-during-commit' $false $false 'cancel-shrink'
    Test-ShrinkPartition 'changed-layout' $false $false 'shrink-layout-changed' 1 'layout changed'
    Test-ShrinkPartition 'revalidation-io' $false $false 'shrink-revalidate' 1 `
        'partition was not changed'
    Test-ShrinkPartition 'dismount-failure' $false $false 'dismount' 1 'partition was not changed'
    foreach ($Fault in @('shrink-before-set', 'shrink-result', 'shrink-flush',
        'shrink-refresh', 'shrink-readback')) {
        Test-ShrinkPartition $Fault $false $false $Fault 1 'its result is uncertain'
    }
    Test-ShrinkPartition 'volume-unavailable' $false $false 'shrink-volume' 1 `
        'the smaller partition and filesystem are synchronized'
}
catch {
    # Keep the test failure visible even if its detach cleanup also fails.
    Write-Warning "Shrink test failed: $_"
    throw
}
finally {
    if ($OwnDetachJobs) {
        Wait-TestDiskImageDetaches $DetachJobs
    }
}
if ($OwnDetachJobs) {
    Write-Host 'windows-shrink-partition: passed'
}
else {
    Write-Host 'windows-shrink-partition: checks passed; background detach cleanup pending'
}
