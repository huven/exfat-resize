# SPDX-License-Identifier: MIT

param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDirectory,

    [Parameter(Mandatory = $true)]
    [string] $OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-CheckedCommand {
    param(
        [string] $Program,
        [string[]] $Arguments
    )

    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$Temporary = Join-Path ([IO.Path]::GetTempPath()) `
    "exfat-resize-windows-build-$([guid]::NewGuid().ToString('N'))"

try {
    if (Test-Path -LiteralPath (Join-Path $SourceDirectory ".git")) {
        throw "Source directory must be an extracted, Git-free source archive"
    }

    $VersionFile = Join-Path $SourceDirectory "VERSION"
    $BuildVersionFile = Join-Path $SourceDirectory ".tarball-version"
    if (-not (Test-Path -LiteralPath $VersionFile -PathType Leaf) -or
        -not (Test-Path -LiteralPath $BuildVersionFile -PathType Leaf)) {
        throw "Source archive version metadata is missing"
    }

    $PackageVersion = (Get-Content -LiteralPath $VersionFile -Raw).Trim()
    $BuildVersion = (Get-Content -LiteralPath $BuildVersionFile -Raw).Trim()
    if ($PackageVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
        throw "Invalid package version: $PackageVersion"
    }
    if ($BuildVersion -notmatch '^[0-9A-Za-z][0-9A-Za-z.+-]*$') {
        throw "Invalid build version: $BuildVersion"
    }
    if ($BuildVersion -ne $PackageVersion -and
        -not $BuildVersion.StartsWith("$PackageVersion-")) {
        throw "Package and build versions disagree"
    }
    if ((Split-Path -Leaf $SourceDirectory) -ne "exfat-resize-$BuildVersion") {
        throw "Source directory and build versions disagree"
    }

    $BuildDirectory = Join-Path $Temporary "build"
    $StageDirectory = Join-Path $Temporary "stage"
    $ArchiveRoot = Join-Path $Temporary "archive"
    $Package = "exfat-resize-$BuildVersion-windows-x86_64"
    $PackageDirectory = Join-Path $ArchiveRoot $Package
    New-Item -ItemType Directory -Path $BuildDirectory, $StageDirectory, $PackageDirectory |
        Out-Null
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

    Invoke-CheckedCommand -Program cmake -Arguments @(
        "-S", $SourceDirectory,
        "-B", $BuildDirectory,
        "-A", "x64",
        "-DEXFAT_RESIZE_BUILD_CLI=ON",
        "-DEXFAT_RESIZE_BUILD_TESTS=OFF",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
    )
    Invoke-CheckedCommand -Program cmake -Arguments @(
        "--build", $BuildDirectory,
        "--config", "Release",
        "--parallel",
        "--target", "exfat-resize"
    )
    Invoke-CheckedCommand -Program cmake -Arguments @(
        "--install", $BuildDirectory,
        "--config", "Release",
        "--component", "Runtime",
        "--prefix", $StageDirectory
    )

    $Binary = Join-Path $StageDirectory "bin/exfat-resize.exe"
    $Documentation = Join-Path $StageDirectory "share/doc/exfat_resize"
    $Contributing = Join-Path $Documentation "CONTRIBUTING.md"
    $License = Join-Path $Documentation "LICENSE"
    $Readme = Join-Path $Documentation "README.md"
    $Transaction = Join-Path $Documentation "docs/TRANSACTION.md"
    foreach ($RequiredFile in @($Binary, $Contributing, $License, $Readme, $Transaction)) {
        if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
            throw "CMake runtime installation is incomplete: $RequiredFile"
        }
    }

    $VersionOutput = & $Binary --version
    if ($LASTEXITCODE -ne 0 -or $VersionOutput -ne "exfat-resize $BuildVersion") {
        throw "Built CLI version does not match the source archive"
    }

    $PackageDocumentation = Join-Path $PackageDirectory "docs"
    New-Item -ItemType Directory -Path $PackageDocumentation | Out-Null
    Copy-Item -LiteralPath $Binary -Destination $PackageDirectory
    Copy-Item -LiteralPath $Contributing -Destination $PackageDirectory
    Copy-Item -LiteralPath $License -Destination $PackageDirectory
    Copy-Item -LiteralPath $Readme -Destination $PackageDirectory
    Copy-Item -LiteralPath $Transaction -Destination $PackageDocumentation

    $Archive = Join-Path $OutputDirectory "$Package.zip"
    if (Test-Path -LiteralPath $Archive) {
        throw "Output archive already exists: $Archive"
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($ArchiveRoot, $Archive)
    Write-Host "Built $Archive"
}
finally {
    if (Test-Path -LiteralPath $Temporary) {
        Remove-Item -LiteralPath $Temporary -Recurse -Force
    }
}
