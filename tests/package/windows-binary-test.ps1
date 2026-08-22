# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)]
    [string] $Archive,

    [Parameter(Mandatory = $true)]
    [string] $SourceDirectory
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

function Find-Dumpbin {
    $Command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $Command) {
        return $Command.Path
    }

    $Vswhere = Join-Path ${env:ProgramFiles(x86)} `
        "Microsoft Visual Studio/Installer/vswhere.exe"
    Assert-Condition (Test-Path -LiteralPath $Vswhere -PathType Leaf) `
        "Could not locate vswhere.exe"
    $VswhereArguments = @(
        "-latest",
        "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath"
    )
    $InstallationPath = ((& $Vswhere @VswhereArguments) -join "").Trim()
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($InstallationPath)) `
        "Could not locate the MSVC tools"

    $DumpbinPattern = Join-Path $InstallationPath `
        "VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe"
    $Candidates = @(Get-ChildItem -Path $DumpbinPattern -File |
        Sort-Object FullName -Descending)
    Assert-Condition ($Candidates.Count -gt 0) "Could not locate dumpbin.exe"
    return $Candidates[0].FullName
}

function Assert-SameFile {
    param(
        [string] $Expected,
        [string] $Actual
    )

    $ExpectedHash = (Get-FileHash -LiteralPath $Expected -Algorithm SHA256).Hash
    $ActualHash = (Get-FileHash -LiteralPath $Actual -Algorithm SHA256).Hash
    Assert-Condition ($ActualHash -eq $ExpectedHash) `
        "Archived file differs from source: $(Split-Path -Leaf $Actual)"
}

$Archive = (Resolve-Path -LiteralPath $Archive).Path
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$BuildVersion = (Get-Content -LiteralPath `
    (Join-Path $SourceDirectory ".tarball-version") -Raw).Trim()
$Package = "exfat-resize-$BuildVersion-windows-x86_64"
Assert-Condition ((Split-Path -Leaf $Archive) -eq "$Package.zip") `
    "Windows archive and source build versions disagree"

$Temporary = Join-Path ([IO.Path]::GetTempPath()) `
    "exfat-resize-windows-test-$([guid]::NewGuid().ToString('N'))"
try {
    $Extracted = Join-Path $Temporary "extracted"
    New-Item -ItemType Directory -Path $Extracted | Out-Null
    Expand-Archive -LiteralPath $Archive -DestinationPath $Extracted

    $TopLevel = @(Get-ChildItem -LiteralPath $Extracted -Force)
    Assert-Condition ($TopLevel.Count -eq 1 -and $TopLevel[0].PSIsContainer -and
        $TopLevel[0].Name -eq $Package) "Windows archive has an unexpected top-level directory"
    $PackageDirectory = $TopLevel[0].FullName

    $Actual = @(Get-ChildItem -LiteralPath $PackageDirectory -Recurse -Force | ForEach-Object {
        $_.FullName.Substring($PackageDirectory.Length + 1).Replace('\', '/')
    } | Sort-Object)
    $Expected = @(
        "CONTRIBUTING.md",
        "docs",
        "docs/PARTITIONING.md",
        "docs/TRANSACTION.md",
        "exfat-resize.exe",
        "LICENSE",
        "README.md"
    ) | Sort-Object
    Assert-Condition (($Actual -join "`n") -eq ($Expected -join "`n")) `
        "Windows archive contains an unexpected file set"

    $Binary = Join-Path $PackageDirectory "exfat-resize.exe"
    $VersionOutput = & $Binary --version
    Assert-Condition ($LASTEXITCODE -eq 0 -and $VersionOutput -eq `
        "exfat-resize $BuildVersion") "Archived CLI version is incorrect"
    & $Binary --help | Out-Null
    Assert-Condition ($LASTEXITCODE -eq 0) "Archived CLI help command failed"

    Assert-SameFile (Join-Path $SourceDirectory "CONTRIBUTING.md") `
        (Join-Path $PackageDirectory "CONTRIBUTING.md")
    Assert-SameFile (Join-Path $SourceDirectory "LICENSE") `
        (Join-Path $PackageDirectory "LICENSE")
    Assert-SameFile (Join-Path $SourceDirectory "README.md") `
        (Join-Path $PackageDirectory "README.md")
    Assert-SameFile (Join-Path $SourceDirectory "docs/PARTITIONING.md") `
        (Join-Path $PackageDirectory "docs/PARTITIONING.md")
    Assert-SameFile (Join-Path $SourceDirectory "docs/TRANSACTION.md") `
        (Join-Path $PackageDirectory "docs/TRANSACTION.md")

    $Dumpbin = Find-Dumpbin
    $Headers = & $Dumpbin /HEADERS $Binary 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "dumpbin could not inspect the archived CLI"
    Assert-Condition (($Headers -join "`n") -match '(?im)^\s*8664 machine \(x64\)\s*$') `
        "Archived CLI is not an x86-64 executable"

    $DependencyOutput = & $Dumpbin /DEPENDENTS $Binary 2>&1
    Assert-Condition ($LASTEXITCODE -eq 0) "dumpbin could not inspect CLI dependencies"
    $Dependencies = @([regex]::Matches(
        ($DependencyOutput -join "`n"),
        '(?im)^\s*([A-Za-z0-9_.-]+\.dll)\s*$'
    ) | ForEach-Object { $_.Groups[1].Value.ToUpperInvariant() } | Sort-Object -Unique)
    Assert-Condition ($Dependencies.Count -gt 0) "Archived CLI has no reported DLL dependencies"
    Assert-Condition ($Dependencies -contains "KERNEL32.DLL") `
        "Archived CLI does not import the Windows system API"
    $RuntimeDependencies = @($Dependencies | Where-Object {
        $_ -match '^(VCRUNTIME|MSVCP|MSVCR|CONCRT|VCOMP).*\.DLL$' -or
        $_ -match '^UCRTBASE(D)?\.DLL$' -or $_ -match '^API-MS-WIN-CRT-'
    })
    Assert-Condition ($RuntimeDependencies.Count -eq 0) `
        "Archived CLI dynamically imports a C/C++ runtime: $RuntimeDependencies"

    Write-Host "Windows binary archive test: passed"
    Write-Host "Imported system DLLs: $($Dependencies -join ', ')"
}
finally {
    if (Test-Path -LiteralPath $Temporary) {
        Remove-Item -LiteralPath $Temporary -Recurse -Force
    }
}
