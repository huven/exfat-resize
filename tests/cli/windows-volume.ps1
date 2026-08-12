# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)]
    [string] $Program,

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
    param([string] $Target)

    $Output = & $Program $Target 2>&1
    $Status = $LASTEXITCODE
    $Output | ForEach-Object { Write-Host $_ }
    Assert-Condition ($Status -eq 0) "exfat-resize failed for $Target (exit $Status)"
    Assert-Condition (($Output -join "`n") -match "exfat-resize: resized") `
        "exfat-resize did not report success for $Target"
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
        Assert-Condition ($Partition.Size -eq 160MB) `
            "Partition size is $($Partition.Size), expected 160 MiB"
        Assert-Condition ($Volume.Size -eq 96MB) `
            "Initial filesystem size is $($Volume.Size), expected 96 MiB"

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
        Invoke-Resize $Target
        $Volume = Wait-Volume $DriveLetter
        Invoke-CleanCheck $DriveLetter
        $Volume = Wait-Volume $DriveLetter
        Assert-Condition ($Volume.Size -eq $Partition.Size) `
            "Filesystem size $($Volume.Size) does not match partition size $($Partition.Size)"

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
            Dismount-DiskImage -ImagePath $ImagePath -StorageType VHDX -ErrorAction Continue
        }
    }
}

$Program = (Resolve-Path -LiteralPath $Program).Path
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
Write-Host "windows-volume: passed"
