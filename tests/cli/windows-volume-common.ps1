# SPDX-License-Identifier: MIT

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

function Start-TestDiskImageDetach {
    param([string] $Image, [System.Collections.Generic.List[object]] $DetachJobs)

    $Job = Dismount-DiskImage -ImagePath $Image -StorageType VHDX -AsJob -ErrorAction Stop
    $DetachJobs.Add([pscustomobject] @{ Image = $Image; Job = $Job })
}

function Wait-TestDiskImageDetaches {
    param([System.Collections.Generic.List[object]] $DetachJobs)

    if ($DetachJobs.Count -eq 0) {
        return
    }
    Write-Host "Waiting for $($DetachJobs.Count) background VHDX detach jobs"
    $DetachJobs.Job | Wait-Job | Out-Null
    $Failures = @()
    foreach ($Detach in $DetachJobs) {
        try {
            Receive-Job -Job $Detach.Job -ErrorAction Stop | Out-Null
            Assert-Condition ($Detach.Job.State -eq 'Completed') `
                "Detach job ended in state $($Detach.Job.State)"
            Assert-Condition (-not (Get-DiskImage -ImagePath $Detach.Image).Attached) `
                'Virtual disk is still attached after Dismount-DiskImage'
        }
        catch {
            $Failures += "$($Detach.Image): $_"
        }
        finally {
            Remove-Job -Job $Detach.Job
        }
    }
    $DetachJobs.Clear()
    Assert-Condition ($Failures.Count -eq 0) "VHDX detach cleanup failed:`n$($Failures -join "`n")"
}
