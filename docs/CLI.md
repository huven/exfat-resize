# Command-line reference

For installation, see [Installing exfat-resize](INSTALLING.md). For practical
grow and shrink workflows, start with the [README](../README.md), including
[Before you resize](../README.md#before-you-resize).

- [Command syntax](#command-syntax)
- [Size argument](#size-argument)
- [Supported targets](#supported-targets)
- [Resizing image files](#resizing-image-files)
- [Safety](#safety)
- [Growing the containing partition on Windows](#growing-the-containing-partition)
- [Shrinking the containing partition on Windows](#shrinking-the-containing-partition)
- [Cancellation](#cancellation)
- [Recovery after failure](#recovery-after-failure)
- [Filesystem compatibility](#filesystem-compatibility)

## Command syntax

```text
exfat-resize device [ size ]
exfat-resize --grow-partition device size
exfat-resize --shrink-partition device size
exfat-resize -h | --help
exfat-resize -V | --version
```

The partition options are available only for Windows logical volumes. They are
mutually exclusive and both require an explicit size. Ordinary filesystem
resizing does not change partition tables or the length of image files.

Run `exfat-resize --help` for a command summary. On macOS and Linux,
`man exfat-resize` also provides a command-line reference.

During the operation, the CLI reports each transaction stage. After successful
completion, it reports the resulting filesystem size in bytes.

Allocation, FAT-chain, and directory validation failures also print a detailed
diagnostic to standard error before the error summary and recovery guidance.
These messages are enabled automatically; no verbose or debug option is needed.
They identify the failed check and relevant cluster numbers or directory-entry
location. Cluster numbers are decimal, directory-entry offsets are in bytes
from the directory's logical start, and raw FAT values and entry types are hexadecimal.
For example:

```text
exfat-resize: cluster 100 is allocated in the bitmap but has no recognized owner (FAT=0xffffffff)
exfat-resize: resize failed: invalid or corrupt exFAT filesystem
exfat-resize: no filesystem write was attempted; correct the error and retry when appropriate
```

The command stops at the first failed check; the diagnostic is not a complete
filesystem-check report. Some failures, including boot validation, I/O, and
memory allocation, retain their existing error messages. Always follow the
recovery guidance, which depends on the stage reached.

The command exits with status zero after success or after showing help or
version information. It exits with status 130 when cancellation is observed
before completion, or another nonzero status on an error.

## Size argument

With no size, the filesystem grows to the available size of its backing image,
device, or partition. That storage must already be large enough; the CLI never
enlarges an image file. On supported Windows logical volumes,
[`--grow-partition`](#growing-the-containing-partition) can enlarge the partition
as part of an explicit-size resize.

A supplied size is the desired final filesystem size, not an amount to add or
remove. Use an unsigned decimal number of bytes, optionally followed by an
uppercase suffix:

| Suffix | Multiplier | Example |
| --- | --- | --- |
| `K` | 1024 | `1024K` = 1 MiB |
| `M` | 1024 squared | `32M` = 32 MiB |
| `G` | 1024 cubed | `512G` = 512 GiB |

The size must be greater than zero and is rounded down to a whole filesystem
sector. A smaller size requests shrink. An equal rounded size is rejected,
except that `--shrink-partition` can finish the partition step when the
filesystem is already at the requested size.

Keep the backing storage at its original size until filesystem shrink succeeds.
After success, a separate tool may truncate the image or reduce only the
partition end. Keep the partition start fixed and retain at least the resulting
filesystem size reported by the CLI. See [partition resizing](PARTITIONING.md).
On Windows, [`--shrink-partition`](#shrinking-the-containing-partition) performs
both steps. `--grow-partition` cannot be combined with a shrink target.

## Supported targets

### macOS and Linux

`device` may be a regular image file or an unmounted raw block device. The
selected target must present the exFAT main boot sector at sector zero: select
the filesystem's partition, rather than a whole disk containing a partition
table. Paths may resolve through symbolic links; verify the resolved target.

Prevent all other access throughout the operation. Do not leave an image
attached through a loop or disk-image device while resizing it. Regular-file
locks are advisory and cannot exclude a process that ignores them. On macOS,
the CLI rejects known mounted or otherwise busy backing objects. Keep the
image or raw device unavailable to other processes until the command finishes.

For growth, first enlarge the partition or other backing storage with the
appropriate platform tool. See [partition resizing](PARTITIONING.md).

### Windows

`device` may be:

- A regular image-file path, including a Unicode path.
- An exact drive designator such as `E:`.
- An exact volume-GUID path such as `\\?\Volume{GUID}\`.

Physical-disk paths such as `\\.\PhysicalDrive0` and other Windows device
namespace forms are not supported as command targets.

Run the CLI from the extracted release directory. An image file must already
have the desired length and is presented to the filesystem as a device with
512-byte sectors. Resizing an image normally does not require Administrator
privileges, but the account running the command must have exclusive read/write
access to it:

```powershell
.\exfat-resize.exe C:\path\to\image.exfat
```

For a logical volume, open an elevated terminal in that directory and close all
files and applications using the volume. The CLI locks the volume exclusively
before reading or writing it, retains the lock for the complete operation,
synchronizes all writes, and dismounts the filesystem before releasing the lock.
If the volume cannot be locked, the resize does not start.

Use either its drive designator or volume-GUID path:

```powershell
.\exfat-resize.exe E:
.\exfat-resize.exe "\\?\Volume{GUID}"
```

These commands grow the filesystem to the existing volume size without changing
its partition table. An explicit size may also be supplied when it fits inside
the existing volume:

```powershell
.\exfat-resize.exe E: 512G
```

## Resizing image files

These procedures apply to an existing **raw image containing one exFAT
filesystem**, with its main boot sector at byte zero. A partitioned whole-disk
image or a VHD/VHDX container cannot be passed directly to the CLI.

Make and verify a backup first. Keep the image detached and unavailable to other
processes while changing its length or running `exfat-resize`. Complete
[the image checks](#checking-an-image) before resizing and after success.
Never shorten an image after a failed or cancelled filesystem resize; follow
[recovery guidance](#recovery-after-failure) instead.

Choose [Linux](#linux-images), [macOS](#macos-images), or
[Windows](#windows-images). The examples add 2 GiB for growth or request a
32 MiB filesystem for shrink; replace the paths and target size as needed.

### Checking an image

On Linux, install exfatprogs using your distribution's package manager, then
check the detached image directly:

```sh
fsck.exfat -n image.exfat
```

On macOS, attach the raw image without mounting it, then check the resulting
device. This attachment is only for checking, not for the resize:

```sh
hdiutil attach -nomount -readonly -imagekey diskimage-class=CRawDiskImage image.exfat
```

Read the device name printed by that command. If it is `/dev/disk5`, use:

```sh
sudo fsck_exfat -n /dev/rdisk5
hdiutil detach /dev/disk5
```

Replace `disk5` with the actual device from the attachment, never a guessed
number. Detach it even if checking reports errors. Require successful checking
and detachment before changing the image's length or running the CLI.

On Windows, CHKDSK checks volumes, not raw image-file paths. Use the Linux
checker on a Linux computer or through Ubuntu in
[Windows Subsystem for Linux (WSL)][wsl-install]. If WSL is not installed, follow
Microsoft's setup instructions, including any restart and initial user setup.
In the Ubuntu terminal, install the checker:

```sh
sudo apt-get update
sudo apt-get install exfatprogs
```

Then, from PowerShell, check `C:\path\to\image.exfat` with:

```powershell
wsl -d Ubuntu -- fsck.exfat -n /mnt/c/path/to/image.exfat
if ($LASTEXITCODE -ne 0) { throw 'Image check failed; resolve errors before continuing.' }
```

Use your actual image path. WSL accesses `C:\` as `/mnt/c/`; see
[Windows and Linux file paths][wsl-files]. This checks the file without mounting
the exFAT filesystem. Resolve any checker errors before resizing. After a
successful shrink, shorten the file as directed below before checking it again.

### Linux images

After [checking the image](#checking-an-image), enlarge its file first, then
grow the filesystem. The `&&` runs the CLI only if enlargement succeeds:

```sh
truncate -s +2G image.exfat && exfat-resize image.exfat
```

For shrink, run the entire block below. It asks for the exact resulting
filesystem size only after a successful resize, then shortens the file:

```sh
if exfat-resize image.exfat 32M; then
    printf 'Enter the exact resulting size in bytes printed above: '
    read -r reported_bytes && truncate -s "$reported_bytes" image.exfat
fi
```

Replace `32M` with your desired smaller filesystem size. At the prompt, copy the
exact byte count printed by the successful resize, without a suffix. After
either successful operation, [check the image again](#checking-an-image) before
mounting it and verifying your files.

### macOS images

Use the [Linux image commands](#linux-images) if `truncate` is available. Stock
macOS 11/12 does not include it. The following alternative uses Python 3 and
works without `truncate` on those versions or newer macOS releases.

Check `python3 --version`. If Python 3 is unavailable, install it using a
[python.org macOS installer][python-macos] compatible with your macOS version,
then open a new terminal. Complete [the image checks](#checking-an-image) first.

To add 2 GiB, open the existing file without discarding its contents and extend
it, then grow its filesystem only if that succeeds:

```sh
python3 - image.exfat <<'PY' && exfat-resize image.exfat
import sys
with open(sys.argv[1], "r+b") as image:
    image.truncate(image.seek(0, 2) + 2 * 1024**3)
PY
```

For shrink, run the entire block. It stops before shortening the file if the
filesystem resize fails or is cancelled, and asks for the exact resulting size:

```sh
if exfat-resize image.exfat 32M; then
    printf 'Enter the exact resulting size in bytes printed above: '
    read -r reported_bytes && python3 - image.exfat "$reported_bytes" <<'PY'
import sys
length = int(sys.argv[2])
with open(sys.argv[1], "r+b") as image:
    if not 0 < length < image.seek(0, 2):
        raise ValueError("Reported size must be positive and smaller than the image file")
    image.truncate(length)
PY
fi
```

Replace `32M` with your desired smaller filesystem size. Copy the exact
**reported byte count**, not a rounded display value, at the prompt. Python's
[`r+b` mode][python-open] rejects a missing file rather than creating one;
[`truncate`][python-truncate] sets the existing file's length in bytes.
After either successful operation, [check the image again](#checking-an-image)
before mounting it and verifying your files.

[wsl-install]: https://learn.microsoft.com/en-us/windows/wsl/install
[wsl-files]: https://learn.microsoft.com/en-us/windows/wsl/filesystems
[python-macos]: https://www.python.org/downloads/macos/
[python-open]: https://docs.python.org/3/library/functions.html#open
[python-truncate]: https://docs.python.org/3/library/io.html#io.IOBase.truncate

### Windows images

These examples use Windows PowerShell 5.1 or newer from the extracted release
folder. Replace the image path with an existing raw image containing just the
exFAT filesystem, and complete [the image checks](#checking-an-image) first.
Keep the image detached and unavailable to other processes. Administrator
privileges are normally unnecessary for a regular file you can read and write.

To add 2 GiB to the image, extend the file first, close its file handle, then grow
the filesystem:

```powershell
$ErrorActionPreference = 'Stop'
$image = 'C:\path\to\image.exfat'
$file = [IO.File]::Open($image, [IO.FileMode]::Open,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    $file.SetLength($file.Length + 2GB)
}
finally {
    $file.Dispose()
}
.\exfat-resize.exe $image
if ($LASTEXITCODE -ne 0) { throw 'Resize failed; follow its recovery guidance.' }
```

To shrink, resize the filesystem successfully **before** shortening the file.
Run the whole block below. It stops on a resize error or cancellation, then asks
for the exact resulting size in bytes printed by the successful command:

```powershell
$ErrorActionPreference = 'Stop'
$image = 'C:\path\to\image.exfat'
.\exfat-resize.exe $image 32M
if ($LASTEXITCODE -ne 0) { throw 'Shrink failed; do not shorten the image.' }
[long]$reportedBytes = Read-Host 'Enter the exact resulting size in bytes printed above'
$file = [IO.File]::Open($image, [IO.FileMode]::Open,
    [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    if ($reportedBytes -le 0 -or $reportedBytes -ge $file.Length) {
        throw 'The reported size must be positive and smaller than the image file.'
    }
    $file.SetLength($reportedBytes)
}
finally {
    $file.Dispose()
}
```

Replace `32M` with your desired smaller filesystem size. Copy the exact **reported
byte count** at the prompt; do not enter the requested size or a rounded display
value. After either successful operation, [check the image again](#checking-an-image)
before attaching it or accessing its files.

These length-changing commands use [.NET File.Open][file-open] with
[`FileMode.Open`][file-mode], so a missing image causes an error rather than
creating a new file. [`SetLength`][file-length] enlarges or truncates only the file;
`exfat-resize` performs the filesystem resize.

Do not apply these file-length commands to VHD or VHDX containers. For those,
[attach the container as a Windows disk][mount-diskimage] and use its exFAT volume
letter with the [volume workflow](../README.md#grow-a-filesystem). Filesystem or
partition shrink does not compact the containing VHD/VHDX file.

[file-open]: https://learn.microsoft.com/en-us/dotnet/api/system.io.file.open?view=netframework-4.8
[file-mode]: https://learn.microsoft.com/en-us/dotnet/api/system.io.filemode?view=netframework-4.8
[file-length]: https://learn.microsoft.com/en-us/dotnet/api/system.io.filestream.setlength?view=netframework-4.8
[mount-diskimage]: https://learn.microsoft.com/en-us/powershell/module/storage/mount-diskimage

## Safety

> [!WARNING]
> Resizing modifies filesystem metadata in place and may result in data loss if
> the operation fails or is interrupted. Make and verify a backup before using
> this tool.

Verify that the supplied path resolves to the intended filesystem and that its
main exFAT boot sector is at sector zero of the selected target.

Before resizing, run an appropriate exFAT filesystem checker while the
filesystem is unmounted and ensure the check completes successfully.
`exfat-resize` is not a general filesystem checker; it validates only the
invariants needed for the resize. The filesystem must also meet the
[compatibility requirements](#filesystem-compatibility).

Prevent all other access for the complete operation. On macOS and Linux, keep
the filesystem unmounted. For a Windows logical volume, close all files and
applications using it; the CLI obtains an exclusive volume lock. When resizing
a regular image file, also ensure it is not attached through a loop or
disk-image device. Platform locking helps detect cooperating users, but
regular-file locks are advisory.

For growth, enlarge the backing object or partition first unless using the
supported Windows `--grow-partition` operation. For shrink, keep the original
backing-storage size until the filesystem resize completes successfully.
The CLI never enlarges or truncates regular files.

A failed or interrupted resize is not automatically repaired, rolled back, or
resumable. Follow the recovery guidance printed with an error; depending on
the stage reached, recovery may require a filesystem checker or restoring the
verified backup. See [Recovery after failure](#recovery-after-failure) before
accessing or retrying a failed resize.

## Growing the containing partition

On supported Windows logical volumes, `--grow-partition` enlarges the containing
partition first if necessary, then grows the filesystem:

```powershell
.\exfat-resize.exe --grow-partition E: 512G
```

This example requests a 512 GiB volume and grows the filesystem to use it. If
the requested size already fits, the partition table is left unchanged and the
filesystem resize proceeds normally.

The option requires:

- An elevated terminal and an explicit size.
- A logical volume that maps to one complete physical-disk extent.
- A basic GPT or MBR data partition with no overlapping layout entry.
- Enough unallocated space immediately after the partition.

Other layouts require enlarging the containing partition separately before
running `exfat-resize`. See [partition resizing](PARTITIONING.md) for external
tools and rescue environments.

Before changing the partition table, the CLI validates both exFAT boot regions,
checks that the filesystem can grow to the requested size, and verifies the
volume-to-disk mapping and physical partition layout. Growth uses Windows'
documented [`IOCTL_DISK_GROW_PARTITION`][windows-grow-partition]. It never moves
a partition start or another partition, and never shrinks a partition.

The full filesystem preflight runs after the partition is enlarged. If that
preflight fails, the original filesystem remains authoritative inside the larger
partition; do not shrink the partition. Correct the reported problem and retry
the filesystem resize. Partition changes require an explicit `--grow-partition`
or `--shrink-partition` option.

If an error reports that a partition update was attempted or the partition was
enlarged, follow the partition-specific guidance even when no filesystem write
was attempted. The CLI attempts to dismount a logical volume after any
partition-update error; prevent further access if that cleanup also fails.
Verify the partition layout before retrying, and never shrink it as a recovery
step.

## Shrinking the containing partition

On supported Windows logical volumes, `--shrink-partition` shrinks the
filesystem, then reduces its containing partition. Run it from an elevated
terminal:

```powershell
.\exfat-resize.exe --shrink-partition E: 384M
```

This option validates the partition layout, shrinks and synchronizes the
filesystem, then reduces only the partition's length using
[`IOCTL_DISK_SET_DRIVE_LAYOUT_EX`][windows-set-layout]. It keeps the start,
partition identity, type, attributes, and other partitions unchanged. The
released tail becomes unallocated disk space. Disk Management and DiskPart
cannot perform this operation for exFAT through their built-in shrink commands.

The target must be a single whole basic GPT data partition or primary MBR
(type 07) partition. Extended/logical, dynamic, spanned, and overlapping layouts
are unsupported. The explicit size is rounded down to a filesystem sector and
must also align to the device sector size. It must be smaller than the partition
and no larger than the filesystem. Neither this option nor ordinary filesystem
shrink reduces the containing VHD/VHDX file or virtual disk capacity.

Before filesystem writes, the CLI validates the partition mapping and allocates
its layout buffers (2 MiB in addition to filesystem workspace). Immediately
before updating the table it checks that the layout still matches. Prevent
other partition-management operations on the disk throughout the command.
See [Windows containing-partition shrink](TRANSACTION.md#windows-containing-partition-shrink)
for the transaction sequence.

If the filesystem succeeds but the partition update has not been attempted,
the error reports a clean smaller filesystem and an unchanged partition.
Remount the volume before access or retrying `--shrink-partition` with the same
size to finish just the partition step. That path validates both clean,
matching boot regions and the partition layout; it does not repeat filesystem
compaction. If a partition update was attempted but synchronization or readback
failed, verify the layout before retrying. These updates are not power-fail
atomic.

Windows can replace the volume device and change its drive letter during the
update. The CLI closes the old handle, verifies the physical layout, and
discovers a fresh volume by its disk extent and partition identity. If the
layout is verified but no matching volume becomes available within five
seconds, it returns an error directing you to refresh or remount and check the
current drive letter. Do not repeat an already completed shrink. After success,
run CHKDSK using the current drive letter.

Windows CHKDSK requires the partition size to match the filesystem size. While
the partition is still larger after an ordinary filesystem shrink, CHKDSK can
report `RAW` even when Windows mounts the filesystem and reads its files.
Complete the partition resize, using `--shrink-partition` or a separate utility,
before checking the smaller filesystem with CHKDSK. See
[Shrinking a containing partition](PARTITIONING.md#shrinking-a-containing-partition).

## Cancellation

On macOS and Linux, Ctrl-C requests cooperative cancellation. On Windows,
Ctrl+C and Ctrl+Break request the same cancellation. Work already in progress
is allowed to reach the next safe boundary. When the request is observed before
completion, the command prints the applicable recovery guidance, performs
normal device cleanup, and exits with status 130. Further cancellation requests
remain handled during cleanup.

Shrink moves allocated tail clusters into free space below the new boundary.
It retains the heap offset and completes each move before observing
cancellation. If cancellation reports a clean original-size filesystem, it
can be used or the shrink can be retried; completed moves remain. Do not
truncate an image or reduce a partition after cancellation.

Cancellation during the final filesystem geometry commit is deferred through
completion, which returns success if all writes succeed. With
`--shrink-partition`, cancellation during compaction leaves the partition
unchanged. Once the filesystem's final commit begins, cancellation is deferred
through the partition update and verification. A request arriving during this
completion returns success if all steps succeed. A partition failure takes
precedence over cancellation.

I/O failures and abnormal termination still require the reported recovery
procedure; individual metadata writes are not power-fail atomic. See the
[resize transaction](TRANSACTION.md) for exact write ordering and durability
guarantees.

## Recovery after failure

Every normal error exit prints stage-specific recovery guidance. Follow that
guidance rather than assuming that rerunning the command is safe. Depending
on the reported state:

- If shrink cancellation left the original-size filesystem clean, use it or
  retry shrink; do not reduce its backing storage.
- If no filesystem write was attempted, correct the reported problem and retry
  when appropriate. For `--grow-partition`, also follow any partition-update
  guidance even when no filesystem write was attempted.
- If the source filesystem remains authoritative, run an appropriate filesystem
  checker before retrying.
- If the filesystem may be incomplete, restore the verified backup.
- If the resize completed but the filesystem's dirty state is uncertain, run an
  appropriate filesystem checker and do not retry the resize.

If the command terminates abnormally without printing stage-specific recovery
guidance, for example because of forced termination, a process crash, or power
loss, assume that destructive metadata updates may have started. Do not retry
the resize; restore the verified backup. Follow a less conservative
checker-and-retry path only when the command explicitly reports that it is safe.

On Windows, forced termination or a process crash may leave the volume
accessible and falsely reported as healthy or clean. Prevent further access
until recovery. For a failed Windows partition operation, also follow the
partition-specific guidance under
[`--grow-partition`](#growing-the-containing-partition) or
[`--shrink-partition`](#shrinking-the-containing-partition). Never reduce
backing storage as a recovery step after an unsuccessful resize.

## Filesystem compatibility

The filesystem must meet these prerequisites:

- Conform to exFAT revision 1.00 with a single FAT; TexFAT/two-FAT volumes are
  not supported.
- Be clean and not have the exFAT media-failure flag set.
- When accessed through a block device, have a filesystem sector size that is
  a multiple of the block device's sector size.
- Not contain Vendor Allocation directory entries, whose allocation semantics
  cannot be interpreted without recognizing their vendor GUID.
- For growth, gain enough clusters to hold the replacement allocation bitmap
  in the newly added tail space.
- For shrink, fit all current allocations, including the full current bitmap,
  below the new heap boundary. The bitmap is shortened only at final commit;
  exceptionally tight targets can therefore be rejected even when a smaller
  final bitmap would fit.
- For growth, not have a source cluster recorded as bad where the expanded FAT
  would overlap it. Bad clusters that remain in the Cluster Heap at the same
  physical location are preserved.

[windows-grow-partition]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntdddisk/ni-ntdddisk-ioctl_disk_grow_partition
[windows-set-layout]: https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-ioctl_disk_set_drive_layout_ex
