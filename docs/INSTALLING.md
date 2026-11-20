# Installation

Choose your platform: [Linux](#linux), [macOS](#macos), or [Windows](#windows).
For other architectures, or to build the CLI and library yourself, see
[Build from source](#build-from-source).

Download prebuilt CLI archives from
[GitHub Releases](https://github.com/huven/exfat-resize/releases). Replace
`X.Y.Z` in the commands below with the release version. GitHub displays an
immutable SHA-256 digest beside each asset; compare it with the checksum from
the downloaded archive before extracting it.

After installation, follow the [resize instructions](../README.md), including
the safety checklist, before changing a filesystem.

## Linux

1. Run `uname -m` to choose the archive for your CPU:

   | Output | Archive |
   | --- | --- |
   | `x86_64` | `exfat-resize-X.Y.Z-linux-x86_64.tar.gz` |
   | `aarch64` or `arm64` | `exfat-resize-X.Y.Z-linux-arm64.tar.gz` |

   Other CPU architectures require a [source build](#build-from-source).

2. Download the archive and verify its SHA-256 checksum. For x86-64:

   ```sh
   sha256sum exfat-resize-X.Y.Z-linux-x86_64.tar.gz
   ```

   Compare the output with GitHub's digest. For ARM64, replace `x86_64` with
   `arm64` in this command and the commands below.

3. Extract and install:

   ```sh
   tar -xzf exfat-resize-X.Y.Z-linux-x86_64.tar.gz
   cd exfat-resize-X.Y.Z-linux-x86_64
   sudo ./install.sh
   exfat-resize --version
   ```

The installer places the CLI, manual page, and documentation under `/usr/local`.
See [custom installation and uninstalling](#custom-installation-and-uninstalling)
to choose another prefix or remove an installation.

## macOS

The prebuilt CLI supports macOS 11 or newer on Apple silicon (ARM64). It is
Developer ID-signed and notarized by Apple. Other architectures require a
[source build](#build-from-source).

1. Download `exfat-resize-X.Y.Z-macos-arm64.tar.gz`. In Safari, Option-click the
   asset link to download it without automatically opening and decompressing it.

2. Verify the retained archive's checksum against GitHub's digest:

   ```sh
   shasum -a 256 exfat-resize-X.Y.Z-macos-arm64.tar.gz
   ```

3. Extract and install:

   ```sh
   tar -xzf exfat-resize-X.Y.Z-macos-arm64.tar.gz
   cd exfat-resize-X.Y.Z-macos-arm64
   sudo ./install.sh
   exfat-resize --version
   ```

The installer places the CLI, manual page, and documentation under `/usr/local`.
See [custom installation and uninstalling](#custom-installation-and-uninstalling)
to choose another prefix or remove an installation.

Apple does not support attaching a notarization ticket directly to a standalone
command-line executable. A Mac therefore needs network access when Gatekeeper
first checks a newly downloaded copy of the macOS binary.

## Windows

The prebuilt CLI supports x86-64 Windows. No installation is required. The
executable is self-contained, with MSVC's C runtime linked statically, so it
does not require a separate Visual C++ Redistributable installation.

1. Download `exfat-resize-X.Y.Z-windows-x86_64.zip`.

2. In PowerShell, calculate its checksum and compare the `Hash` with GitHub's
   SHA-256 digest:

   ```powershell
   Get-FileHash exfat-resize-X.Y.Z-windows-x86_64.zip -Algorithm SHA256
   ```

3. Extract the archive and run the CLI from its directory:

   ```powershell
   Expand-Archive exfat-resize-X.Y.Z-windows-x86_64.zip -DestinationPath .
   cd exfat-resize-X.Y.Z-windows-x86_64
   .\exfat-resize.exe --version
   ```

The archive also contains the license, README, contribution guide, and detailed
documentation. Resizing an image file normally does not require elevation. To
access a logical volume or change its partition, open PowerShell or Command
Prompt as Administrator, change to the extracted directory, and follow the
[resize instructions](../README.md). See the [CLI reference](CLI.md) for
supported Windows targets and partition options.

## Build from source

Install Git, CMake 3.20 or newer, and a C11 compiler. On Windows, use a terminal
where the compiler is available. On macOS, if CMake.app is installed without
command-line links, first add its tools to `PATH`:

```sh
export PATH=/Applications/CMake.app/Contents/bin:$PATH
```

Clone, build, and install:

```sh
git clone https://github.com/huven/exfat-resize.git
cd exfat-resize
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXFAT_RESIZE_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release
```

These commands build and install both the CLI and library on Linux, macOS, and
Windows. On other platforms, only the portable library is built by default.
The install command may require elevated privileges depending on the
destination prefix. To choose another prefix:

```sh
cmake --install build --config Release --prefix /your/prefix
```

The full install includes the CLI, static library, public header, CMake package
files, and a manual page on Linux and macOS. Documentation and the exact MIT
license are installed under CMake's `CMAKE_INSTALL_DOCDIR`, with guides in its
`docs` subdirectory so their relative links continue to work.

See [Building and linking](LIBRARY.md#building-and-linking) for library-only
builds and CMake integration. For development builds and tests, follow
[Contributing](../CONTRIBUTING.md).

## Custom installation and uninstalling

For Linux and macOS prebuilt archives, `install.sh` defaults to `/usr/local`.
To use another absolute prefix, set `PREFIX`. Elevated privileges are
unnecessary when the destination is writable:

```sh
PREFIX=/your/prefix ./install.sh
```

Use the same prefix and the `uninstall.sh` from the same release to remove the
installed files. For the default prefix:

```sh
sudo ./uninstall.sh
```

For a custom prefix:

```sh
PREFIX=/your/prefix ./uninstall.sh
```

## Binary compatibility details

The Linux x86-64 and ARM64 binaries are statically linked with musl and built
as position-independent executables (PIE). They require no installed C library
or dynamic loader, so the same executable can run on glibc-based and
musl-based distributions. They are built on Alpine 3.22 and tested by
performing exFAT resizes on Debian 12, Ubuntu 22.04 LTS, and Alpine 3.22 for
each architecture.

For kernel compatibility,
[musl documents Linux 2.6.39 or newer](https://wiki.musl-libc.org/supported-platforms)
as its baseline for POSIX-conformant behavior. Updates to the bundled C library
arrive with new binary releases.
