# exfat-resize

exfat-resize provides:

- A portable C11 library for growing existing exFAT filesystems.
- A command-line tool (thin wrapper around the library) for Linux, macOS,
  and Windows.

## Quick start

### Prebuilt CLI binaries

Prebuilt CLI binaries are provided for these platforms:

- Linux x86-64 with glibc
- Windows x86-64

Download the prebuilt CLI from
[GitHub Releases](https://github.com/huven/exfat-resize/releases) and verify the
SHA-256 digest shown by GitHub. On Linux:

    tar -xzf exfat-resize-X.Y.Z-linux-x86_64-glibc.tar.gz
    cd exfat-resize-X.Y.Z-linux-x86_64-glibc
    sudo ./install.sh

On Windows PowerShell, no installation is required:

    Expand-Archive exfat-resize-X.Y.Z-windows-x86_64.zip -DestinationPath .
    cd exfat-resize-X.Y.Z-windows-x86_64
    .\exfat-resize.exe --help

See [Installing prebuilt CLI binaries](#installing-prebuilt-cli-binaries) for
platform compatibility, checksum verification, custom installation prefixes,
and platform-specific setup.

### Build from source

Building requires CMake 3.20 or newer and a C11 compiler.

    git clone https://github.com/huven/exfat-resize.git
    cd exfat-resize
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXFAT_RESIZE_BUILD_TESTS=OFF
    cmake --build build --config Release
    cmake --install build --config Release

The commands build and install both the CLI and library on Linux, macOS, and
Windows. On other platforms, only the portable library is built by default.

The install command may require elevated privileges depending on the destination
prefix.

Before using the tool, read [Safety](#safety), then see [Usage](#usage)
for supported targets and examples.

## Safety

> [!WARNING]
> Resizing modifies filesystem metadata in place and may result in data loss if
> the operation fails or is interrupted. Make and verify a backup before using
> this tool.

Verify that the supplied path resolves to the intended filesystem. Paths may
resolve through symbolic links, and the selected device must present the exFAT
main boot sector at sector zero. The CLI does not modify partition tables unless
the Windows-only `--grow-partition` option is explicitly supplied.

Before resizing, run an appropriate exFAT filesystem checker while the
filesystem is unmounted and ensure the check completes successfully.
`exfat-resize` is not a general filesystem checker; it validates only the
invariants needed for the resize.

Prevent all other access for the complete operation. On macOS and Linux, keep
the filesystem unmounted. For a Windows logical volume, close all files and
applications using it; the CLI obtains an exclusive volume lock, retains it
while accessing the volume, and dismounts the filesystem before releasing the
lock. When resizing a regular image file, also ensure it is not attached through
a loop or disk-image device. Platform locking helps detect cooperating users,
but regular-file locks are advisory and cannot exclude a process that ignores
them.

The backing object or partition must already be enlarged unless a supported
Windows logical volume is used with `--grow-partition`. The tool never enlarges
regular files. A failed or interrupted resize is not automatically repaired,
rolled back, or resumable. Follow the recovery guidance printed with an error;
depending on the stage reached, recovery may require a filesystem checker or
restoring the verified backup.

Every normal error exit prints stage-specific recovery guidance. If the command
terminates abnormally without printing stage-specific recovery guidance, for
example because of an interruption, process crash, or power loss, assume that
destructive metadata updates may have started; do not retry the resize, and
restore the verified backup. Follow a less conservative checker-and-retry path
only when the command explicitly reports that it is safe.

If `--grow-partition` reports that a partition update was attempted or that the
partition was enlarged, follow its partition-specific guidance even though no
filesystem write was attempted. The CLI attempts to dismount a logical volume
after any partition-update error; prevent further access if that cleanup also
fails. Verify the partition layout before retrying, and never shrink it as a
recovery step.

## Usage

    exfat-resize device [ size ]
    exfat-resize -h | --help
    exfat-resize -V | --version

Run `exfat-resize --help` for a command summary. On macOS and Linux,
`man exfat-resize` provides the complete command-line reference.

With no size, the filesystem grows to the available size of the backing object.
A specified size is the desired filesystem size as an unsigned number of bytes.
It may have an uppercase `K`, `M`, or `G` suffix, which multiplies the number by
1024, 1024 squared, or 1024 cubed, respectively. The size is rounded down to a
whole filesystem sector. Shrinking is not supported.

The backing image, device, or partition must provide enough space for the
requested size before the command runs. The CLI never enlarges an image file.
On Windows, [`--grow-partition`](#growing-the-containing-partition) can
optionally enlarge a supported partition as part of an explicit-size resize.

Only filesystems meeting the
[compatibility requirements](#filesystem-compatibility) below are supported.

### macOS and Linux

`device` may be a regular image file or an unmounted raw block device. Prevent
all other access to it for the complete operation. In particular, do not leave
an image attached through a loop or disk-image device while resizing it.

Enlarge a regular file containing an exFAT filesystem, then grow the filesystem
to use all available space:

    truncate -s +2G image.exfat
    exfat-resize image.exfat

After enlarging the partition or other backing storage with the appropriate
platform tool, grow the filesystem on an unmounted raw device:

    exfat-resize /dev/your-exfat-device

Regular-file locks are advisory and cannot exclude a process that ignores them.
On macOS, the CLI rejects known mounted or otherwise busy backing objects. Keep
the image or raw device unavailable to other processes until the command has
finished.

### Windows

`device` may be:

- A regular image-file path, including a Unicode path.
- An exact drive designator such as `E:`.
- An exact volume-GUID path such as `\\?\Volume{GUID}\`.

Physical-disk paths such as `\\.\PhysicalDrive0` and other Windows device
namespace forms are not supported as command targets.

An image file must already have the desired length and is presented to the
filesystem as a device with 512-byte sectors. Resizing an image normally does
not require Administrator privileges, although the account running the command
must have exclusive read/write access to the file:

    exfat-resize C:\path\to\image.exfat

For a logical volume, open an elevated terminal and close all files and
applications using it. The CLI locks the volume exclusively before reading or
writing it, retains the lock for the complete operation, synchronizes all
writes, and dismounts the filesystem before releasing the lock. If the volume
cannot be locked, the resize does not start.

Use either its drive designator or volume-GUID path:

    exfat-resize E:
    exfat-resize \\?\Volume{GUID}\

These commands grow the filesystem to the existing volume size; they do not
change its partition table. An explicit size may also be supplied when it fits
inside the existing volume:

    exfat-resize E: 512G

#### Growing the containing partition

If an explicit size does not fit inside a logical Windows volume,
`--grow-partition` asks the CLI to enlarge the containing partition first:

    exfat-resize --grow-partition E: 512G

This example requests a 512 GiB volume and grows the filesystem to use it. If
the requested size already fits, the partition table is left unchanged and the
filesystem resize proceeds normally.

Partition growth is deliberately opt-in because changing a partition table has
a wider effect than changing metadata within the selected filesystem. It
requires:

- An elevated terminal and an explicit size.
- A logical volume that maps to one complete physical-disk extent.
- A basic GPT or MBR data partition with no overlapping layout entry.
- Enough unallocated space immediately after the partition.

Before changing the partition table, the CLI validates both exFAT boot regions,
checks that the filesystem can grow to the requested size, and verifies the
volume-to-disk mapping and physical partition layout. It then uses Windows'
documented [`IOCTL_DISK_GROW_PARTITION`][windows-grow-partition]. It never moves
a partition start or another partition, and never shrinks a partition.

The full filesystem preflight runs after the partition is enlarged. If that
preflight fails, the original filesystem remains authoritative inside the
larger partition; do not shrink the partition. Correct the reported problem
and retry the filesystem resize. Without `--grow-partition`, the CLI never
changes a partition table.

For a layout that this mode does not support, one open-source alternative is to
boot [GParted Live][gparted-live], identify the whole target disk carefully, and
use the included terminal rather than GParted's graphical exFAT resize action:

```text
sudo parted /dev/sdX
(parted) unit s
(parted) print free
(parted) resizepart PARTITION_NUMBER NEW_END_SECTOR
(parted) quit
```

[GNU Parted documents][resizepart] that `resizepart` changes the partition end
without modifying the filesystem. Record the start sector first, select only
an end sector in the immediately following free extent, and verify afterwards
that the start is unchanged. Do not use this procedure to shrink exFAT.

Windows DiskPart is not a substitute: its [documented `extend`
operation][diskpart-extend] automatically extends NTFS and fails without a
partition change for other formatted filesystems. Do not rely on claims that it
will silently extend only an exFAT partition.

[diskpart-extend]: https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/extend
[gparted-live]: https://gparted.org/livecd.php
[resizepart]: https://www.gnu.org/software/parted/manual/html_node/resizepart.html
[windows-grow-partition]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntdddisk/ni-ntdddisk-ioctl_disk_grow_partition

## Filesystem compatibility

The filesystem must meet these prerequisites:

- Use exFAT revision 1.00 with a single FAT; TexFAT/two-FAT volumes are not
  supported.
- Be clean and not have the exFAT media-failure flag set.
- When accessed through a block device, have a filesystem sector size that is
  a multiple of the block device's sector size.
- Not contain Vendor Allocation directory entries, whose allocation semantics
  cannot be interpreted without recognizing their vendor GUID.
- Gain enough clusters to hold the replacement allocation bitmap in the newly
  added tail space.
- Not have a source cluster recorded as bad where the expanded FAT would
  overlap it. Bad clusters that remain in the Cluster Heap at the same physical
  location are preserved.

## Supported platforms

The core library is designed to be portable across mainstream C11 environments
and has no operating-system dependencies. It is continuously tested with GCC
and Clang on Linux, Apple Clang on macOS, and MSVC on Windows.

The bundled CLI
is a thin platform-specific wrapper around the library:

| Platform | Image files | Direct filesystem access | Partition growth |
| --- | --- | --- | --- |
| macOS | Yes | Raw block devices | Use an external partitioning tool |
| Linux | Yes | Raw block devices | Use an external partitioning tool |
| Windows | Yes | Drive designators and volume-GUID paths | `--grow-partition` for supported layouts |

## C library

The repository also builds a C11 library with the public C-facing header
`<exfat_resize.h>`. The core accepts a sector-addressed device and does not
depend on file descriptors, disk images, raw-device APIs, or another filesystem
implementation. Callback sector numbers use the device sector size declared by
the caller. The library reads the filesystem sector size from the exFAT boot
sector and adapts I/O between the two sector sizes; the filesystem sector size
must be a multiple of the callback device sector size.
Sector zero supplied by the caller must be the exFAT main boot sector; the
library does not parse partition tables or apply the boot sector's
`PartitionOffset`.

Read and write callbacks always receive a nonzero, in-range transfer. Their
buffer has capacity for at least the requested sector count, and they must
access only the requested bytes and either complete the full transfer or
report failure. The library handles zero-sector operations itself without
invoking a callback. The installed header defines the complete callback
contract, including synchronization and lifetime requirements.

The caller supplies exclusive access, a target size in bytes, and allocation
callbacks:

```c
static void *allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void deallocate(void *context, void *memory, size_t size)
{
    (void)context;
    (void)size;
    free(memory);
}

struct exfat_resize_options options = {
    .allocator = {
        .allocate = allocate,
        .deallocate = deallocate
    }
};

enum exfat_resize_stage stage;
error = exfat_resize(&device, target_size, &options, &stage);
```

Before writing, the library snapshots the used source FAT and rebuilds a
validated in-memory allocation model covering the target cluster heap. See the
transaction document's [memory requirements](docs/TRANSACTION.md#memory-requirements)
for allocator-backed working-memory sizes, lifetimes, and backing options.

When an error is returned, `stage` describes the recovery boundary reached:

- `PREFLIGHT`: no write was attempted;
- `PREPARING`: the source layout remains authoritative; run a filesystem
  checker before retrying;
- `RESIZING`: authoritative source metadata may have been overwritten; restore
  the verified backup;
- `FINALIZING`: the resized target was synchronized, but its final dirty state
  is uncertain; run a filesystem checker and do not retry the resize.

On success, `stage` is `COMPLETED`. The stage pointer may be `NULL` when the
caller does not need recovery guidance. See
[docs/TRANSACTION.md](docs/TRANSACTION.md) for the exact write ordering and
failure guarantees.

Downstream CMake projects can use `add_subdirectory()` and link
`exfat_resize::exfat_resize`. The install target also provides the header,
static library, CLI executable, CMake package files, and a manual page on macOS
and Linux. This README and the exact MIT license are installed under CMake's
`CMAKE_INSTALL_DOCDIR`.
Installed CMake packages use same-major version compatibility, so consumers
can require a compatible 1.x release:

    find_package(exfat_resize 1.0 CONFIG REQUIRED)

The public C API remains source compatible throughout the 1.x series. Build
consumers with the header and static library from the same release and with
ABI-compatible compiler settings for the target platform. Compatibility
between separately compiled artifacts from different releases or deliberately
different compiler ABIs is not promised. Incompatible source changes require a
new major release.

## Installing prebuilt CLI binaries

### Linux

GitHub Releases provide a prebuilt CLI archive expected to run on all conventional
`x86_64` Linux distributions using glibc 2.28 or newer. It is built on
AlmaLinux 8.10 and tested by performing an exFAT resize on Debian 12 and Ubuntu
22.04 LTS. Musl-based distributions such as Alpine, non-FHS systems such as
NixOS, and other CPU architectures require a different build or compatibility
setup.

Download `exfat-resize-X.Y.Z-linux-x86_64-glibc.tar.gz` from the corresponding
[GitHub Release](https://github.com/huven/exfat-resize/releases). GitHub displays
an immutable SHA-256 digest beside the asset; compare it with the output of:

    sha256sum exfat-resize-X.Y.Z-linux-x86_64-glibc.tar.gz

Replace `X.Y.Z` below with the release version, then extract and install it:

    tar -xzf exfat-resize-X.Y.Z-linux-x86_64-glibc.tar.gz
    cd exfat-resize-X.Y.Z-linux-x86_64-glibc
    sudo ./install.sh

The installer defaults to `/usr/local`. To use another absolute prefix, set
`PREFIX`; elevated privileges are unnecessary when the destination is writable:

    PREFIX=/your/prefix ./install.sh

Use the same prefix and the `uninstall.sh` from the same release to remove the
installed files:

    sudo ./uninstall.sh

For a custom prefix, remove it with:

    PREFIX=/your/prefix ./uninstall.sh

### Windows

GitHub Releases provide a self-contained x86-64 Windows CLI archive. The
executable is built with MSVC's C runtime linked statically, so it does not
require a separate Visual C++ Redistributable installation.

Download `exfat-resize-X.Y.Z-windows-x86_64.zip` from the corresponding
[GitHub Release](https://github.com/huven/exfat-resize/releases). GitHub displays
an immutable SHA-256 digest beside the asset; compare it with the `Hash` shown
by PowerShell:

    Get-FileHash exfat-resize-X.Y.Z-windows-x86_64.zip -Algorithm SHA256

Replace `X.Y.Z` with the release version, then extract the archive and run the
CLI from the extracted directory:

    Expand-Archive exfat-resize-X.Y.Z-windows-x86_64.zip -DestinationPath .
    cd exfat-resize-X.Y.Z-windows-x86_64
    .\exfat-resize.exe --version

The archive also contains the license, this README, and the resize-transaction
documentation. Resizing an image file normally does not require elevation. To
access a logical volume or grow its partition, open PowerShell or Command Prompt
as Administrator and invoke the extracted executable as described under
[Windows usage](#windows).

## Build and test

CMake 3.20 or newer is the canonical build system for the library, CLI, and
tests.

### macOS and Linux

The top-level Makefile is a convenience wrapper around CMake and the release
scripts. If the macOS CMake application is installed without command-line
links, add its tools to `PATH`:

    export PATH=/Applications/CMake.app/Contents/bin:$PATH

    make
    make test
    make sanitize-test
    make release-test

`make` produces `build/exfat-resize`. `make test` builds every target and runs
the separately labeled library, package, and CLI suites through CTest. The CLI
tests use newfs_exfat and fsck_exfat on macOS, or mkfs.exfat and fsck.exfat on
Linux. The sanitizer target runs all suites with AddressSanitizer and
UndefinedBehaviorSanitizer in a separate build directory. It also builds and
runs C and C++ `add_subdirectory()` consumers to verify the sanitized static
library's runtime link requirements.
The release target creates and verifies the source archive from committed
`HEAD`, installs it into a temporary prefix, and builds C and C++ consumers
using both `find_package()` and `add_subdirectory()`.

For a custom installation prefix:

    cmake --install build --prefix /your/prefix

### Windows build

The Windows build includes the CLI and library by default. It uses only native
Windows APIs and does not require a POSIX compatibility layer:

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build --build-config Release --output-on-failure -L "library|cli|package-native"
    cmake --install build --config Release
