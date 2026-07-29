# exfat-resize

exfat-resize provides a portable C11 library and command-line tool for growing
an existing exFAT filesystem in a regular file or raw block device. The backing
object must be enlarged before the filesystem is resized.

## Safety

> [!WARNING]
> Resizing modifies filesystem metadata in place and may result in data loss if
> the operation fails or is interrupted. Make and verify a backup before using
> this tool.

Verify that the supplied path resolves to the intended filesystem. Paths may
resolve through symbolic links, and the selected device must present the exFAT
main boot sector at sector zero; the tool does not inspect or modify partition
tables.

Before resizing, run an appropriate exFAT filesystem checker while the
filesystem is unmounted and ensure the check completes successfully.
`exfat-resize` is not a general filesystem checker; it validates only the
invariants needed for the resize.

Keep the filesystem unmounted and prevent all other access for the complete
operation. When resizing a regular image file, also ensure it is not attached
through a loop or disk-image device. Platform locking helps detect cooperating
users, but regular-file locks are advisory and cannot exclude a process that
ignores them.

The backing object or partition must already be enlarged. The tool does not
enlarge regular files or partitions. A failed or interrupted resize is not
automatically repaired, rolled back, or resumable. Follow the recovery guidance
printed with an error; depending on the stage reached, recovery may require a
filesystem checker or restoring the verified backup.

If the command terminates without printing stage-specific recovery guidance,
for example because of an interruption, process crash, or power loss, assume
that destructive metadata updates may have started. Do not retry the resize;
restore the verified backup. Follow a less conservative checker-and-retry path
only when the command explicitly reports that it is safe.

Only filesystems meeting the
[compatibility requirements](#filesystem-compatibility) below are supported.

    exfat-resize device [ size ]
    exfat-resize -h | --help
    exfat-resize -V | --version

After installation, `man exfat-resize` provides the complete command-line
reference, including operational safety and recovery guidance.

With no size, the filesystem grows to the available size of the backing object.
A specified size is the desired filesystem size as an unsigned number of
bytes. It is rounded down to a whole filesystem sector. Shrinking is not
supported.

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

macOS and Linux are currently supported and tested. Contributions adding
support for other platforms are welcome.

The CLI implements synchronization barriers with `F_FULLFSYNC` for regular
images and `DKIOCSYNCHRONIZE` for raw devices on macOS, and with `fsync` on
Linux. Persistence ultimately depends on the operating system and storage
device honoring their flush requests.
On macOS, platform mechanisms reject known mounted or otherwise busy backing
objects. The CLI retains its requested locks for the complete operation;
regular-file locks remain advisory as described above.

## Examples

Enlarge a regular file containing an exFAT filesystem, then grow the filesystem
to use all available space:

    truncate -s +2G image.exfat
    exfat-resize image.exfat

Grow the exFAT filesystem on an unmounted raw device to use all available space:

    exfat-resize /dev/your-exfat-device

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

Before writing, the library rebuilds and validates an in-memory allocation
model covering the target cluster heap. See the transaction document's
[memory requirements](docs/TRANSACTION.md#memory-requirements) for
allocator-backed working-memory sizes, lifetimes, and backing options.

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
static library, CLI executable, manual page, and CMake package files. This
README and the exact MIT license are installed under CMake's
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

## Build and test

CMake 3.20 or newer is the canonical build system for the library, CLI, and
tests. The top-level Makefile is a convenience wrapper around CMake and the
release scripts. If the macOS CMake application is installed without
command-line links, add its tools to `PATH`:

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

To create a self-contained source archive:

    make dist

The archive and its SHA-256 checksum are written to `dist/`. The archive is
created from the committed source at `HEAD`; uncommitted changes are not
included. Repeated builds of the same commit produce byte-identical archives.
