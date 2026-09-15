# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)]
    [string] $Program,

    [Parameter(Mandatory = $true)]
    [string] $FaultProgram,

    [Parameter(Mandatory = $true)]
    [string] $Fixture,

    [Parameter(Mandatory = $true)]
    [string] $ExpectedHash
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Assert-Condition {
    param(
        [bool] $Condition,
        [string] $Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Get-FixtureManifest {
    param([string] $Root)

    $Root = (Get-Item -LiteralPath $Root -Force).FullName.TrimEnd('\', '/') +
        [System.IO.Path]::DirectorySeparatorChar
    # Windows may add unrelated root entries such as System Volume Information.
    # Every entry in the fixture's own namespace is included, including hidden files.
    $Entries = @(
        foreach ($Entry in Get-ChildItem -LiteralPath $Root -Force) {
            if ($Entry.Name -in @('payload.bin', 'small-files')) {
                # Enumeration retains on-disk casing, unlike a FileInfo made from
                # a caller-supplied path on Windows' case-insensitive filesystem.
                $Entry
                if ($Entry.PSIsContainer) {
                    Get-ChildItem -LiteralPath $Entry.FullName -Force -Recurse
                }
            }
        }
    )
    foreach ($Entry in $Entries) {
        $Entry.Refresh()
        # Capture metadata before hashing. LastAccessTime is deliberately omitted:
        # reads of a mounted exFAT volume can change it. These are native Windows
        # timestamps, not a comparison of raw exFAT timestamp/UTC-offset encoding.
        $Row = [pscustomobject] @{
            Path = $Entry.FullName.Substring($Root.Length).Replace('\', '/')
            Type = $(if ($Entry.PSIsContainer) { 'directory' } else { 'file' })
            Length = $(if ($Entry.PSIsContainer) { 0 } else { $Entry.Length })
            SHA256 = ''
            CreationUtc = $Entry.CreationTimeUtc.Ticks
            ModificationUtc = $Entry.LastWriteTimeUtc.Ticks
            # The exFAT read-only, hidden, system, directory and archive bits.
            Attributes = ([int] $Entry.Attributes -band 0x37)
        }
        if (-not $Entry.PSIsContainer) {
            $Row.SHA256 = (Get-FileHash -LiteralPath $Entry.FullName -Algorithm SHA256).Hash
        }
        $Row
    }
}

function Assert-ManifestEqual {
    param(
        [object[]] $Expected,
        [object[]] $Actual,
        [string] $Context,
        [string[]] $Properties = @(
            'Path', 'Type', 'Length', 'SHA256', 'CreationUtc', 'ModificationUtc', 'Attributes'
        )
    )

    $Differences = @(Compare-Object -ReferenceObject $Expected -DifferenceObject $Actual `
        -Property $Properties -CaseSensitive)
    Assert-Condition ($Differences.Count -eq 0) `
        "$Context differs: $($Differences | ConvertTo-Json -Compress)"
}

function Assert-InitialFixture {
    param(
        [object[]] $Manifest,
        [string] $PayloadHash
    )

    $SHA256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Expected = @(
            [pscustomobject] @{ Path = 'payload.bin'; Type = 'file';
                Length = 8MB; SHA256 = $PayloadHash }
            [pscustomobject] @{ Path = 'small-files'; Type = 'directory';
                Length = 0; SHA256 = '' }
            for ($Index = 1; $Index -le 200; $Index++) {
                # Keep in sync with prepare-windows-volume.sh (UTF-8, LF, no BOM).
                $Bytes = [System.Text.Encoding]::UTF8.GetBytes(
                    "exfat-resize Windows volume fixture $Index`n")
                [pscustomobject] @{
                    Path = "small-files/$Index.txt"
                    Type = 'file'
                    Length = $Bytes.Length
                    SHA256 = [BitConverter]::ToString($SHA256.ComputeHash($Bytes)).Replace('-', '')
                }
            }
        )
    }
    finally {
        $SHA256.Dispose()
    }
    Assert-ManifestEqual $Expected $Manifest 'Initial fixture' `
        @('Path', 'Type', 'Length', 'SHA256')
}

function Test-ManifestOracle {
    param([string] $Root)

    $SmallFiles = Join-Path $Root 'small-files'
    New-Item -ItemType Directory -Path $SmallFiles -Force | Out-Null
    $Payload = Join-Path $Root 'payload.bin'
    $SmallFile = Join-Path $SmallFiles '1.txt'
    [System.IO.File]::WriteAllText($Payload, 'payload')
    [System.IO.File]::WriteAllText($SmallFile, 'small')
    try {
        $Baseline = @(Get-FixtureManifest $Root)
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest $Root) 'Unchanged oracle fixture'
        $Mutations = @(
            @{ Name = 'same-length content'; Property = 'SHA256'; Apply = {
                $Entry = Get-Item -LiteralPath $SmallFile
                $Created = $Entry.CreationTimeUtc
                $Modified = $Entry.LastWriteTimeUtc
                [System.IO.File]::WriteAllText($SmallFile, 'other')
                # Leave size and stable metadata unchanged, so only the hash can detect this.
                [System.IO.File]::SetCreationTimeUtc($SmallFile, $Created)
                [System.IO.File]::SetLastWriteTimeUtc($SmallFile, $Modified)
            } },
            @{ Name = 'creation time only'; Property = 'CreationUtc'; Apply = {
                $Entry = Get-Item -LiteralPath $Payload
                $Entry.CreationTimeUtc = $Entry.CreationTimeUtc.AddSeconds(4)
            } },
            @{ Name = 'modification time only'; Property = 'ModificationUtc'; Apply = {
                $Entry = Get-Item -LiteralPath $Payload
                $Entry.LastWriteTimeUtc = $Entry.LastWriteTimeUtc.AddSeconds(4)
            } },
            @{ Name = 'attributes only'; Property = 'Attributes'; Apply = {
                $Entry = Get-Item -LiteralPath $Payload
                $Entry.Attributes = $Entry.Attributes -bor [System.IO.FileAttributes]::ReadOnly
            } },
            @{ Name = 'rename'; Property = 'Path'; Apply = {
                Rename-Item -LiteralPath $SmallFile -NewName 'renamed.txt'
            } },
            @{ Name = 'case-only root rename'; Property = 'Path'; Apply = {
                Rename-Item -LiteralPath $Payload -NewName 'Payload.bin' -Force
            } }
        )
        foreach ($Mutation in $Mutations) {
            $Baseline = @(Get-FixtureManifest $Root)
            & $Mutation.Apply
            $Actual = @(Get-FixtureManifest $Root)
            if ($Mutation.Property -ne 'Path') {
                $Unchanged = @('Path', 'Type', 'Length', 'SHA256', 'CreationUtc',
                    'ModificationUtc', 'Attributes') | Where-Object { $_ -ne $Mutation.Property }
                Assert-ManifestEqual $Baseline $Actual "Unrelated fields for $($Mutation.Name)" `
                    $Unchanged
            }
            $Rejected = $false
            try {
                Assert-ManifestEqual $Baseline $Actual "Oracle $($Mutation.Name)"
            }
            catch {
                $Rejected = $true
            }
            Assert-Condition $Rejected "Manifest oracle accepted $($Mutation.Name) mutation"
            $FieldChanges = @(Compare-Object $Baseline $Actual `
                -Property $Mutation.Property -CaseSensitive)
            Assert-Condition ($FieldChanges.Count -gt 0) `
                "Oracle $($Mutation.Name) did not change $($Mutation.Property)"
        }
    }
    finally {
        Remove-Item -LiteralPath $Root -Recurse -Force
    }
    Write-Host 'windows manifest oracle: passed'
}

function Wait-Volume {
    param([char] $DriveLetter)

    for ($Index = 0; $Index -lt 50; $Index++) {
        Get-Item "$DriveLetter`:\" -ErrorAction SilentlyContinue | Out-Null
        $Volume = Get-Volume -DriveLetter $DriveLetter -ErrorAction SilentlyContinue
        if ($null -ne $Volume -and $Volume.OperationalStatus -contains "OK") {
            return $Volume
        }
        Start-Sleep -Milliseconds 100
    }
    throw "Volume $DriveLetter`: did not become ready"
}

function Invoke-CleanCheck {
    param([char] $DriveLetter)

    & chkdsk.exe "$DriveLetter`:" /F /X
    Assert-Condition ($LASTEXITCODE -eq 0) `
        "chkdsk reported errors for volume $DriveLetter`: (exit $LASTEXITCODE)"
}

function Invoke-Resize {
    param(
        [string] $Target,
        [uint64] $TargetSize,
        [bool] $ExpectPartitionGrowth = $true
    )

    $Output = & $Program --grow-partition $Target $TargetSize 2>&1
    $Status = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Status -eq 0) "exfat-resize failed for $Target (exit $Status)"
    Assert-Condition (($Output -join "`n") -match "exfat-resize: resized") `
        "exfat-resize did not report success for $Target"
    if ($ExpectPartitionGrowth) {
        Assert-Condition (($Output -join "`n") -match "grew the partition") `
            "exfat-resize did not report partition growth for $Target"
    } else {
        Assert-Condition (($Output -join "`n") -notmatch "grew the partition") `
            "exfat-resize unexpectedly reported partition growth for $Target"
    }
}

function Invoke-ExpectedFailure {
    param(
        [string[]] $Arguments,
        [string] $ExpectedText
    )

    $Output = & $Program @Arguments 2>&1
    $Status = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Status -ne 0) `
        "exfat-resize unexpectedly succeeded with arguments: $Arguments"
    Assert-Condition (($Output -join "`n") -match [regex]::Escape($ExpectedText)) `
        "exfat-resize did not report '$ExpectedText'"
    Assert-Condition (($Output -join "`n") -notmatch 'exfat-resize: [A-Za-z]::') `
        "exfat-resize printed duplicate punctuation after a drive designator"
}

function Test-PartitionFailure {
    param(
        [string] $ImagePath,
        [string] $Fault,
        [string] $ExpectedError,
        [string] $ExpectedGuidance,
        [string] $PayloadHash,
        [bool] $DismountFailure
    )

    $Mounted = $false
    try {
        $DiskImage = Mount-DiskImage `
            -ImagePath $ImagePath -StorageType VHDX -Access ReadWrite -PassThru
        $Mounted = $true
        $Disk = $DiskImage | Get-Disk
        $Partition = @(Get-Partition -DiskNumber $Disk.Number |
            Where-Object { $_.Type -ne "Reserved" })[0]
        if ([string]::IsNullOrWhiteSpace([string] $Partition.DriveLetter)) {
            $Partition | Add-PartitionAccessPath -AssignDriveLetter
            $Partition = Get-Partition -DiskNumber $Disk.Number `
                -PartitionNumber $Partition.PartitionNumber
        }
        $DriveLetter = [char] $Partition.DriveLetter
        Wait-Volume $DriveLetter | Out-Null

        $env:EXFAT_RESIZE_TEST_PARTITION_FAULT = $Fault
        try {
            $TargetSize = [string] ([uint64] 160MB)
            $Output = & $FaultProgram --grow-partition "$DriveLetter`:" $TargetSize 2>&1
            $Status = $LASTEXITCODE
        }
        finally {
            Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue
        }
        $Combined = $Output -join "`n"
        $Output | ForEach-Object { Write-Host $_ }
        Assert-Condition ($Status -ne 0) "Fault '$Fault' unexpectedly succeeded"
        Assert-Condition ($Combined -match [regex]::Escape($ExpectedError)) `
            "Fault '$Fault' did not report '$ExpectedError'"
        Assert-Condition ($Combined -match [regex]::Escape($ExpectedGuidance)) `
            "Fault '$Fault' did not report conservative partition guidance"
        if ($DismountFailure) {
            Assert-Condition ($Combined -match 'cannot dismount the volume after resizing') `
                "Fault '$Fault' did not report the secondary dismount failure"
            Assert-Condition ($Combined -match 'the volume may remain mounted') `
                "Fault '$Fault' did not report the mounted-volume precaution"
        } else {
            Assert-Condition ($Combined -match 'exfat-resize-test: dismounted the volume') `
                "Fault '$Fault' did not dismount the volume before releasing its lock"
        }
        Assert-Condition ($Combined -notmatch 'no filesystem write was attempted') `
            "Fault '$Fault' incorrectly reported that no update was attempted"
        if ($Fault -match 'cancel-grow') {
            Assert-Condition ($Combined -notmatch 'interrupted by user') `
                "Fault '$Fault' masked the partition error with cancellation"
        }

        Update-HostStorageCache
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        Assert-Condition ($Partition.Size -eq 160MB) `
            "Fault '$Fault' did not leave the expected enlarged partition"
        $Volume = Wait-Volume $DriveLetter
        Assert-Condition ($Volume.FileSystem -eq "exFAT") `
            "Fault '$Fault' did not leave a mountable exFAT volume"
        $ActualHash = (Get-FileHash "$DriveLetter`:\payload.bin" -Algorithm SHA256).Hash
        Assert-Condition ($ActualHash -eq $PayloadHash) `
            "Payload differs after partition fault '$Fault'"
        Write-Host "windows partition fault ($Fault): passed"
    }
    finally {
        Set-Location $env:GITHUB_WORKSPACE
        if ($Mounted) {
            Dismount-DiskImage -ImagePath $ImagePath -StorageType VHDX -ErrorAction Continue |
                Out-Null
        }
    }
}

function Test-PartitionCancellation {
    param(
        [string] $ImagePath,
        [string] $CancellationPoint,
        [bool] $ExpectPartitionGrowth,
        [string] $PayloadHash
    )

    $Mounted = $false
    try {
        $DiskImage = Mount-DiskImage `
            -ImagePath $ImagePath -StorageType VHDX -Access ReadWrite -PassThru
        $Mounted = $true
        $Disk = $DiskImage | Get-Disk
        $Partition = @(Get-Partition -DiskNumber $Disk.Number |
            Where-Object { $_.Type -ne "Reserved" })[0]
        if ([string]::IsNullOrWhiteSpace([string] $Partition.DriveLetter)) {
            $Partition | Add-PartitionAccessPath -AssignDriveLetter
            $Partition = Get-Partition -DiskNumber $Disk.Number `
                -PartitionNumber $Partition.PartitionNumber
        }
        $DriveLetter = [char] $Partition.DriveLetter
        $Volume = Wait-Volume $DriveLetter
        $InitialPartitionSize = [uint64] $Partition.Size
        $InitialFileSystemSize = [uint64] $Volume.Size

        $env:EXFAT_RESIZE_TEST_PARTITION_FAULT = $CancellationPoint
        try {
            $TargetSize = [string] ([uint64] 160MB)
            $Output = & $FaultProgram --grow-partition "$DriveLetter`:" $TargetSize 2>&1
            $Status = $LASTEXITCODE
        }
        finally {
            Remove-Item Env:EXFAT_RESIZE_TEST_PARTITION_FAULT -ErrorAction SilentlyContinue
        }
        $Combined = $Output -join "`n"
        $Output | ForEach-Object { Write-Host $_ }
        Assert-Condition ($Status -eq 130) `
            "Cancellation at '$CancellationPoint' returned status $Status instead of 130"
        Assert-Condition ($Combined -match 'interrupted by user') `
            "Cancellation at '$CancellationPoint' did not report interruption"
        Assert-Condition ($Combined -match 'no filesystem write was attempted') `
            "Cancellation at '$CancellationPoint' did not report no filesystem write"
        Assert-Condition ($Combined -notmatch 'checking filesystem') `
            "Cancellation at '$CancellationPoint' entered the resize library"
        Assert-Condition ($Combined -match 'exfat-resize-test: dismounted the volume') `
            "Cancellation at '$CancellationPoint' did not dismount the volume"

        Update-HostStorageCache
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        if ($ExpectPartitionGrowth) {
            Assert-Condition ($Partition.Size -eq 160MB) `
                "Cancellation at '$CancellationPoint' did not retain partition growth"
            Assert-Condition ($Combined -match 'the partition was enlarged') `
                "Cancellation at '$CancellationPoint' omitted partition-grown guidance"
        } else {
            Assert-Condition ($Partition.Size -eq $InitialPartitionSize) `
                "Cancellation at '$CancellationPoint' unexpectedly changed the partition"
            Assert-Condition ($Combined -notmatch 'the partition was enlarged') `
                "Cancellation at '$CancellationPoint' reported partition growth"
        }
        $Volume = Wait-Volume $DriveLetter
        Assert-Condition ($Volume.FileSystem -eq "exFAT") `
            "Cancellation at '$CancellationPoint' left an unrecognized filesystem"
        Assert-Condition ([uint64] $Volume.Size -eq $InitialFileSystemSize) `
            "Cancellation at '$CancellationPoint' changed the filesystem size"
        $ActualHash = (Get-FileHash "$DriveLetter`:\payload.bin" -Algorithm SHA256).Hash
        Assert-Condition ($ActualHash -eq $PayloadHash) `
            "Payload differs after cancellation at '$CancellationPoint'"
        Write-Host "windows partition cancellation ($CancellationPoint): passed"
    }
    finally {
        Set-Location $env:GITHUB_WORKSPACE
        if ($Mounted) {
            Dismount-DiskImage -ImagePath $ImagePath -StorageType VHDX -ErrorAction Continue |
                Out-Null
        }
    }
}

function Test-VolumeTarget {
    param(
        [string] $ImagePath,
        [ValidateSet("DriveLetter", "VolumeGuid")]
        [string] $TargetType,
        [string] $PayloadHash
    )

    $Mounted = $false
    try {
        $DiskImage = Mount-DiskImage `
            -ImagePath $ImagePath -StorageType VHDX -Access ReadWrite -PassThru
        $Mounted = $true
        $Disk = $DiskImage | Get-Disk
        $Partitions = @(Get-Partition -DiskNumber $Disk.Number |
            Where-Object { $_.Type -ne "Reserved" })
        Assert-Condition ($Partitions.Count -eq 1) `
            "Expected one data partition on disk $($Disk.Number), got $($Partitions.Count)"
        $Partition = $Partitions[0]
        if ([string]::IsNullOrWhiteSpace([string] $Partition.DriveLetter)) {
            $Partition | Add-PartitionAccessPath -AssignDriveLetter
            $Partition = Get-Partition -DiskNumber $Disk.Number `
                -PartitionNumber $Partition.PartitionNumber
        }

        $DriveLetter = [char] $Partition.DriveLetter
        $Volume = Wait-Volume $DriveLetter
        Assert-Condition ($Volume.FileSystem -eq "exFAT") `
            "Volume $DriveLetter`: is not exFAT"
        Assert-Condition ($Partition.Size -eq 96MB) `
            "Partition size is $($Partition.Size), expected 96 MiB"
        # Get-Volume.Size is usable cluster capacity, not the exFAT VolumeLength.
        $InitialFileSystemSize = [uint64] $Volume.Size
        Assert-Condition (
            $InitialFileSystemSize -ge 58MB -and $InitialFileSystemSize -le 68MB
        ) "Initial usable filesystem capacity is $InitialFileSystemSize, expected about 62 MiB"
        Assert-Condition (($Disk.Size - $Partition.Offset - $Partition.Size) -ge 60MB) `
            "Disk does not leave enough trailing space to test partition growth"
        Write-Host "Initial usable filesystem capacity: $InitialFileSystemSize bytes"

        $Root = "$DriveLetter`:\"
        $Baseline = @(Get-FixtureManifest $Root)
        Assert-InitialFixture $Baseline $PayloadHash

        if ($TargetType -eq "DriveLetter") {
            $Target = "$DriveLetter`:"
        } else {
            $Target = ((& mountvol.exe "$DriveLetter`:" /L) |
                Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
                Select-Object -First 1).Trim()
            Assert-Condition ($Target -match '^\\\\\?\\Volume\{[0-9A-Fa-f-]+\}\\$') `
                "mountvol returned an unexpected volume name: $Target"
        }

        Set-Location $env:GITHUB_WORKSPACE
        $TargetSize = [uint64] 160MB
        Invoke-ExpectedFailure `
            -Arguments @($Target, [string] $TargetSize) `
            -ExpectedText "request is outside the backing device"
        $Volume = Wait-Volume $DriveLetter
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        Assert-Condition ($Partition.Size -eq 96MB) `
            "Partition changed without --grow-partition"

        if ($TargetType -eq "DriveLetter") {
            Invoke-ExpectedFailure `
                -Arguments @(
                    "--grow-partition", $Target, [string] ([uint64] (64MB + 1))
                ) `
                -ExpectedText "target does not add enough usable clusters"
            $Partition = Get-Partition -DiskNumber $Disk.Number `
                -PartitionNumber $Partition.PartitionNumber
            Assert-Condition ($Partition.Size -eq 96MB) `
                "Partition changed for a target that cannot grow the filesystem"

            Invoke-ExpectedFailure `
                -Arguments @("--grow-partition", $Target, [string] ([uint64] 256MB)) `
                -ExpectedText "not enough immediately trailing unallocated space"
            $Volume = Wait-Volume $DriveLetter
            $Partition = Get-Partition -DiskNumber $Disk.Number `
                -PartitionNumber $Partition.PartitionNumber
            Assert-Condition ($Partition.Size -eq 96MB) `
                "Partition changed after an out-of-space failure"
        }

        Assert-ManifestEqual $Baseline @(Get-FixtureManifest $Root) 'Fixture before resizing'
        $OriginalPartitionSize = [uint64] $Partition.Size
        if ($TargetType -eq "DriveLetter") {
            $UnalignedIncrement = [uint64] 1
        } else {
            $UnalignedIncrement = [uint64] 511
        }
        $UnalignedTargetSize = $OriginalPartitionSize + $UnalignedIncrement
        Invoke-Resize `
            -Target $Target `
            -TargetSize $UnalignedTargetSize `
            -ExpectPartitionGrowth $false
        Update-HostStorageCache
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        Assert-Condition ($Partition.Size -eq $OriginalPartitionSize) `
            "Partition changed for a target that rounds down to its existing size"
        $Volume = Wait-Volume $DriveLetter
        # Compare before chkdsk /F can repair or otherwise modify the evidence.
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest $Root) `
            "Fixture after filesystem growth through $TargetType"
        Invoke-CleanCheck $DriveLetter
        $Volume = Wait-Volume $DriveLetter
        $IntermediateFileSystemSize = [uint64] $Volume.Size
        Write-Host "Intermediate usable filesystem capacity: $IntermediateFileSystemSize bytes"
        Assert-Condition ($IntermediateFileSystemSize -ge ($InitialFileSystemSize + 20MB)) `
            "Filesystem did not grow into the existing partition"
        Assert-Condition ($IntermediateFileSystemSize -le $Partition.Size) `
            "Filesystem capacity exceeds the unchanged partition size"
        Assert-Condition (($Partition.Size - $IntermediateFileSystemSize) -le 4MB) `
            "Filesystem leaves more than 4 MiB of the unchanged partition unavailable"

        Invoke-Resize $Target $TargetSize
        Update-HostStorageCache
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        Assert-Condition ($Partition.Size -eq $TargetSize) `
            "Partition size is $($Partition.Size), expected $TargetSize"
        $Volume = Wait-Volume $DriveLetter
        Assert-ManifestEqual $Baseline @(Get-FixtureManifest $Root) `
            "Fixture after partition growth through $TargetType"
        Invoke-CleanCheck $DriveLetter
        $Volume = Wait-Volume $DriveLetter
        $FinalFileSystemSize = [uint64] $Volume.Size
        Write-Host "Final usable filesystem capacity: $FinalFileSystemSize bytes"
        Assert-Condition ($FinalFileSystemSize -ge ($InitialFileSystemSize + 60MB)) `
            "Filesystem capacity grew by less than 60 MiB"
        Assert-Condition ($FinalFileSystemSize -le $Partition.Size) `
            "Filesystem capacity exceeds partition size"
        Assert-Condition (($Partition.Size - $FinalFileSystemSize) -le 4MB) `
            "Filesystem leaves more than 4 MiB of the partition unavailable"

        Write-Host "windows-volume ($TargetType): passed"
    }
    finally {
        Set-Location $env:GITHUB_WORKSPACE
        if ($Mounted) {
            Dismount-DiskImage -ImagePath $ImagePath -StorageType VHDX -ErrorAction Continue |
                Out-Null
        }
    }
}

$Program = (Resolve-Path -LiteralPath $Program).Path
$FaultProgram = (Resolve-Path -LiteralPath $FaultProgram).Path
$Fixture = (Resolve-Path -LiteralPath $Fixture).Path
$PayloadHash = (Get-Content -LiteralPath $ExpectedHash -Raw).Trim().ToUpperInvariant()
$Temporary = Join-Path $env:RUNNER_TEMP "exfat-resize-windows-volume"
New-Item -ItemType Directory -Path $Temporary -Force | Out-Null
Test-ManifestOracle (Join-Path $Temporary "manifest-oracle-$([guid]::NewGuid())")

$DriveImage = Join-Path $Temporary "drive-letter.vhdx"
$GuidImage = Join-Path $Temporary "volume-guid.vhdx"
Copy-Item -LiteralPath $Fixture -Destination $DriveImage -Force
Copy-Item -LiteralPath $Fixture -Destination $GuidImage -Force

Test-VolumeTarget $DriveImage DriveLetter $PayloadHash
Test-VolumeTarget $GuidImage VolumeGuid $PayloadHash

$FaultCases = @(
    @("grow-result", "cannot grow the partition", "a partition update was attempted", $false),
    @(
        "flush",
        "cannot synchronize the enlarged partition table",
        "the partition was enlarged",
        $false
    ),
    @("refresh", "cannot refresh the enlarged physical disk", "the partition was enlarged", $false),
    @("readback", "cannot identify the volume partition", "the partition was enlarged", $false),
    @(
        "refresh,dismount",
        "cannot refresh the enlarged physical disk",
        "the partition was enlarged",
        $true
    ),
    @(
        "cancel-grow,flush",
        "cannot synchronize the enlarged partition table",
        "the partition was enlarged",
        $false
    )
)
foreach ($Case in $FaultCases) {
    $FaultImage = Join-Path $Temporary "partition-fault-$($Case[0]).vhdx"
    Copy-Item -LiteralPath $Fixture -Destination $FaultImage -Force
    Test-PartitionFailure $FaultImage $Case[0] $Case[1] $Case[2] $PayloadHash $Case[3]
}

$CancellationCases = @(
    @("cancel-discovery", $false),
    @("cancel-grow", $true)
)
foreach ($Case in $CancellationCases) {
    $CancellationImage = Join-Path $Temporary "partition-cancellation-$($Case[0]).vhdx"
    Copy-Item -LiteralPath $Fixture -Destination $CancellationImage -Force
    Test-PartitionCancellation $CancellationImage $Case[0] $Case[1] $PayloadHash
}
Write-Host "windows-volume: passed"
exit 0
