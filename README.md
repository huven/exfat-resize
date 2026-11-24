# exfat-resize

Grow and shrink exFAT filesystems on Windows, Linux, and macOS.

[Install](#install) · [Prepare](#before-you-resize) · [Grow](#grow-a-filesystem) ·
[Shrink](#shrink-a-filesystem) · [Image files](#resize-an-image-file) ·
[Troubleshooting](#if-something-goes-wrong)

> [!WARNING]
> Resizing changes data in place. Failure or interruption can cause data loss.
> Make and verify a backup, then complete [Before you resize](#before-you-resize)
> before running any resize command.

## Quick examples

After [installing](#install) and [preparing the filesystem](#before-you-resize):

| Platform | Grow to the partition's available size | Shrink to 64 GiB |
| --- | --- | --- |
| Linux | `sudo exfat-resize /dev/sdb1` | `sudo exfat-resize /dev/sdb1 64G` |
| macOS | `sudo exfat-resize /dev/rdisk4s1` | `sudo exfat-resize /dev/rdisk4s1 64G` |
| Windows | `.\exfat-resize.exe E:` | `.\exfat-resize.exe E: 64G` |

**Replace these example targets with your own exFAT partition or drive.** The
shrink examples assume the filesystem is currently larger than 64 GiB.
On Windows, use an Administrator terminal in the extracted release directory.

The size is the **desired final filesystem size**, not an amount to add or remove.
Uppercase `K`, `M`, and `G` mean KiB, MiB, and GiB; without a suffix, use bytes.
Omitting the size grows the filesystem to fill its existing partition or image.

Changing the filesystem and changing its containing partition are separate steps:

- **Grow:** enlarge the partition first, then grow the filesystem.
- **Shrink:** shrink the filesystem successfully first, then reduce the partition.
- **Windows:** the optional partition commands below can perform both steps.

Follow the [grow](#grow-a-filesystem) or [shrink](#shrink-a-filesystem) workflow
to complete the operation.

## Install

Download the archive for your platform from
[GitHub Releases](https://github.com/huven/exfat-resize/releases). Follow its
installation guide to verify the checksum, extract it, and run or install the CLI:

| Platform | Prebuilt binaries | Instructions |
| --- | --- | --- |
| Linux | x86-64 and ARM64 | [Linux installation](docs/INSTALLING.md#linux) |
| macOS | Apple silicon, macOS 11 or newer | [macOS installation](docs/INSTALLING.md#macos) |
| Windows | x86-64 | [Windows setup](docs/INSTALLING.md#windows) |

For other architectures or a source checkout, see
[Build from source](docs/INSTALLING.md#build-from-source).
The installation guide also covers custom prefixes and uninstalling.

## Before you resize

1. **Make and verify a backup.** There is no automatic rollback or resume.
2. **Identify the exFAT partition carefully.** Check its size and identity using
   `lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT` on Linux, `diskutil list` on macOS, or
   `Get-Volume` in Windows PowerShell. On Linux and macOS, select the partition
   containing exFAT, not the whole disk. Substitute your own target in every
   command below.
3. **Close applications using it, unmount it, and check it for errors.** Use the
   commands for your platform below. Resolve any checker errors before resizing;
   `exfat-resize` does not replace a filesystem checker.
4. **Keep other processes away until resizing finishes.** On Linux and macOS,
   keep the filesystem unmounted. On Windows, keep files and applications closed;
   the CLI requires exclusive access. Detach image files from any loop or
   disk-image device before resizing them. Prevent concurrent partition changes.

| Platform | Unmount and check before resizing |
| --- | --- |
| Linux | `sudo umount /dev/sdb1`, then `sudo fsck.exfat -n /dev/sdb1` |
| macOS | `diskutil unmount /dev/disk4s1`, then `sudo fsck_exfat -n /dev/rdisk4s1` |
| Windows | In an Administrator terminal: `chkdsk E: /f /x` |

On Linux, `fsck.exfat` is supplied by exfatprogs. The examples use `-n` to check
without making repairs. Windows `/x` dismounts the volume and `/f` repairs errors.
See [filesystem requirements](docs/LIBRARY.md#filesystem-compatibility) for supported
exFAT formats and other restrictions.
For image files, follow [image preparation and checking](docs/CLI.md#checking-an-image)
instead of the partition commands above.

## Grow a filesystem

1. Complete [Before you resize](#before-you-resize).
2. **Enlarge the containing partition first.** It needs free space immediately
   after it; preserve its start. Follow the
   [partition growth guide](docs/PARTITIONING.md). If the partition is already
   larger than the filesystem, skip this step.
3. Run your platform's **grow command** from [Quick examples](#quick-examples).
   Without a size, it uses all space in the enlarged partition.
4. After success, check the filesystem again with your platform's checker above,
   then mount it and verify your files.

On Windows, you can instead enlarge a supported partition and its filesystem
together, specifying the final size:

```powershell
.\exfat-resize.exe --grow-partition E: 128G
```

This requires a basic GPT or MBR partition with enough immediately following
unallocated space. Read the [Windows partition requirements and failure
guidance](docs/CLI.md#growing-the-containing-partition) before using this option.

## Shrink a filesystem

1. Complete [Before you resize](#before-you-resize).
2. **Keep the partition at its original size.** Run your platform's **shrink
   command** from [Quick examples](#quick-examples), replacing `64G` with the
   desired smaller size. If the target cannot hold the existing data and required
   filesystem metadata, choose a larger target.
3. **Only after success**, use a separate partitioning tool to reduce the
   partition's end. Keep its start fixed and retain at least the filesystem size
   reported by the CLI. Follow the [partition shrink
   guide](docs/PARTITIONING.md#shrinking-a-containing-partition).
4. Check the filesystem again, then mount it and verify your files. On Windows,
   complete the partition resize to exactly the reported filesystem size before
   running CHKDSK; a size mismatch can make it report `RAW`.

On Windows, you can instead shrink a supported partition and its filesystem
together:

```powershell
.\exfat-resize.exe --shrink-partition E: 64G
```

This supports single whole basic GPT data partitions and primary MBR partitions.
Read the [Windows partition requirements and failure
guidance](docs/CLI.md#shrinking-the-containing-partition). After success, check
the current drive letter before running CHKDSK; Windows may have changed it.

## Resize an image file

Use the path to a raw image containing a single exFAT filesystem. The CLI does
not resize partitioned disk images or VHD/VHDX containers directly.

**Grow:** enlarge the file first, then grow its filesystem. **Shrink:** resize
the filesystem first, then truncate the file only after success, using the exact
reported size. Keep the image detached and unavailable to other processes.

Follow the complete commands for your platform, including image checks:

- [Linux images](docs/CLI.md#linux-images).
- [macOS images](docs/CLI.md#macos-images), including a fallback for macOS 11/12
  without `truncate`.
- [Windows images](docs/CLI.md#windows-images), using PowerShell to change the
  file length. Resizing normally does not require Administrator privileges.

## If something goes wrong

- **Follow the recovery guidance printed by the command.** Do not reduce a
  partition or truncate an image after a failed or cancelled resize.
- To cancel, use Ctrl-C and allow cleanup to finish. Cancellation may take time;
  do not force termination. See [cancellation](docs/CLI.md#cancellation).
- After a crash, power loss, or forced termination without recovery guidance,
  prevent further access, **do not retry**, and restore the verified backup.
  A volume that still mounts may need recovery.
- If a partition update was attempted, verify its layout before retrying. Never
  shrink a partition as a recovery step.

See [Recovery after failure](docs/CLI.md#recovery-after-failure) for the full
instructions, including Windows partition failures.

## Documentation

- [Installation](docs/INSTALLING.md): downloads, source builds, custom prefixes,
  and uninstalling.
- [CLI reference](docs/CLI.md): options, supported targets, compatibility,
  Windows partition operations, and recovery. For a command summary, run
  `exfat-resize --help`; on Linux and macOS, use `man exfat-resize` for the manual.
- [Partition resizing](docs/PARTITIONING.md): changing the containing partition
  with external tools.
- [C library](docs/LIBRARY.md): embedding the portable C11 library and CMake
  integration.
- [Resize transaction](docs/TRANSACTION.md): implementation, durability, and
  memory requirements.
- [Contributing](CONTRIBUTING.md): development setup and tests.
