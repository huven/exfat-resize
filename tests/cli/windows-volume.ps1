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
        [uint64] $TargetSize
    )

    $Output = & $Program --grow-partition $Target $TargetSize 2>&1
    $Status = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Status -eq 0) "exfat-resize failed for $Target (exit $Status)"
    Assert-Condition (($Output -join "`n") -match "exfat-resize: resized") `
        "exfat-resize did not report success for $Target"
    Assert-Condition (($Output -join "`n") -match "grew the partition") `
        "exfat-resize did not report partition growth for $Target"
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
            $InitialFileSystemSize -ge 90MB -and $InitialFileSystemSize -le 100MB
        ) "Initial usable filesystem capacity is $InitialFileSystemSize, expected about 94 MiB"
        Assert-Condition (($Disk.Size - $Partition.Offset - $Partition.Size) -ge 60MB) `
            "Disk does not leave enough trailing space to test partition growth"
        Write-Host "Initial usable filesystem capacity: $InitialFileSystemSize bytes"

        $ActualHash = (Get-FileHash "$DriveLetter`:\payload.bin" -Algorithm SHA256).Hash
        Assert-Condition ($ActualHash -eq $PayloadHash) "Initial payload hash differs"
        Assert-Condition (
            @(Get-ChildItem "$DriveLetter`:\small-files" -File).Count -eq 200
        ) "Initial small-file count differs"

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
                    "--grow-partition", $Target, [string] ([uint64] ($Partition.Size + 1))
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

        Invoke-Resize $Target $TargetSize
        Update-HostStorageCache
        $Partition = Get-Partition -DiskNumber $Disk.Number `
            -PartitionNumber $Partition.PartitionNumber
        Assert-Condition ($Partition.Size -eq $TargetSize) `
            "Partition size is $($Partition.Size), expected $TargetSize"
        $Volume = Wait-Volume $DriveLetter
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

        $ActualHash = (Get-FileHash "$DriveLetter`:\payload.bin" -Algorithm SHA256).Hash
        Assert-Condition ($ActualHash -eq $PayloadHash) `
            "Payload hash differs after resizing through $TargetType"
        Assert-Condition (
            @(Get-ChildItem "$DriveLetter`:\small-files" -File).Count -eq 200
        ) "Small-file count differs after resizing through $TargetType"
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
    )
)
foreach ($Case in $FaultCases) {
    $FaultImage = Join-Path $Temporary "partition-fault-$($Case[0]).vhdx"
    Copy-Item -LiteralPath $Fixture -Destination $FaultImage -Force
    Test-PartitionFailure $FaultImage $Case[0] $Case[1] $Case[2] $PayloadHash $Case[3]
}
Write-Host "windows-volume: passed"
exit 0
