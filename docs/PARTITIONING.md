# Partition resizing outside exfat-resize

`exfat-resize` is focused on resizing exFAT filesystems correctly, not on
general disk partitioning. Windows provides two explicit exceptions:
[`--grow-partition`](CLI.md#growing-the-containing-partition) enlarges a
basic GPT or MBR partition into immediately trailing unallocated space;
[`--shrink-partition`](CLI.md#shrinking-the-containing-partition) reduces
its end after successful filesystem shrink. Otherwise `exfat-resize` never
changes a partition table.

For growth, follow [growpart](#growpart) or
[GNU Parted](#gnu-parted-in-a-live-environment), then return to
[Grow a filesystem](../README.md#grow-a-filesystem). For shrink, follow
[Shrinking a containing partition](#shrinking-a-containing-partition).

When the containing partition is too small, enlarge it first with a suitable
partitioning tool, then run `exfat-resize`. The following are starting points,
not a guide to every disk layout. Make a verified backup, keep the exFAT filesystem
unmounted, never move its start sector, and never shrink it during growth or
failure recovery.

## growpart

[`growpart`][growpart] extends one partition to the end of the disk or the
beginning of the next partition. It supports a dry run:

```text
sudo growpart --dry-run /dev/sdX PARTITION_NUMBER
sudo growpart /dev/sdX PARTITION_NUMBER
```

Here `/dev/sdX` is the whole disk and `PARTITION_NUMBER` selects the partition
on it. For example, partition 1 on `/dev/sdb` is `/dev/sdb1`; pass the partition
path to `exfat-resize`, not the whole disk. Verify the resulting partition
layout before running `exfat-resize`.

## GNU Parted in a live environment

These commands use **GNU Parted's terminal utility** to enlarge the partition;
`exfat-resize` grows the filesystem afterwards. You can run them from a Linux
installation or a live environment. [GParted Live][gparted-live] is one such
environment for x86-64 computers. Open its terminal to run GNU Parted; the
commands below are not instructions for the GParted graphical application.

Identify the whole target disk carefully, then select its partition number and
the new end sector from the layout printed by `print free`:

```text
sudo parted /dev/sdX
(parted) unit s
(parted) print free
(parted) resizepart PARTITION_NUMBER NEW_END_SECTOR
(parted) quit
```

[GNU Parted documents][resizepart] that `resizepart` changes the partition end
without modifying the filesystem. Select only an end sector in the immediately
following free extent and verify afterwards that the start is unchanged.

## Other architectures

For other architectures, use a GNU/Linux installer or rescue image built for
the target hardware. [Debian installer rescue mode][debian-rescue] and
[Ubuntu Server alternative-architecture images][ubuntu-server] are starting
points. Use an available partitioning utility from its shell, or attach the
disk to another supported computer. Confirm that the selected image boots the
hardware and provides the required utility before relying on it.

## Windows DiskPart

Windows DiskPart is not a substitute: its [documented `extend`
operation][diskpart-extend] automatically extends NTFS and fails without a
partition change for other formatted filesystems. Its [documented `shrink`
operation][diskpart-shrink] supports NTFS and unformatted volumes, not exFAT.
Use the explicit `exfat-resize --shrink-partition DEVICE SIZE` option for
supported basic partitions, or a separate partitioning utility.

[debian-rescue]: https://www.debian.org/releases/stable/installmanual
[diskpart-shrink]: https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/shrink
[diskpart-extend]: https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/extend
[gparted-live]: https://gparted.org/livecd.php
[growpart]: https://github.com/canonical/cloud-utils
[resizepart]: https://www.gnu.org/software/parted/manual/html_node/resizepart.html
[ubuntu-server]: https://ubuntu.com/download/server

## Shrinking a containing partition

Keep the original partition size while running `exfat-resize DEVICE SIZE` with
an explicit smaller filesystem target. Only after successful completion may a
separate partitioning utility reduce the partition end. Preserve the start
sector and leave at least the exact resulting filesystem size reported by the
CLI; account for the partitioning utility's sector units and alignment.

On Windows, prefer [`--shrink-partition`](CLI.md#shrinking-the-containing-partition)
for supported partitions. To perform the partition step separately, use GNU
Parted from Linux or a [live environment](#gnu-parted-in-a-live-environment).
For a disk used with macOS or Windows, boot a suitable live environment or attach
the disk to a Linux computer. Reidentify the disk there; its device name can
change between computers and boots.

1. After the filesystem shrink succeeds, record the exact resulting size in
   **bytes** printed by `exfat-resize`. Keep the filesystem unmounted.
2. Open GNU Parted on the **whole disk**, replacing `/dev/sdX` with your disk:

   ```text
   sudo parted /dev/sdX
   (parted) unit s
   (parted) print
   ```

   Record the target partition number, its start sector, and the **logical**
   sector size from `Sector size (logical/physical)`. Do not assume 512 bytes.
3. Calculate the required partition length and its inclusive end sector:

   ```text
   required sectors = filesystem bytes / logical sector bytes, rounded up
   new end sector = start sector + required sectors - 1
   ```

   For example, a successfully shrunk 64 GiB filesystem reports `68719476736`
   bytes. With 512-byte logical sectors and a partition starting at sector
   `2048`, it needs `134217728` sectors and ends at sector `134219775`.
   Use your actual reported size and start, not these example numbers.
4. Change only the partition end, then inspect the layout:

   ```text
   (parted) resizepart PARTITION_NUMBER NEW_END_SECTORs
   (parted) print free
   (parted) quit
   ```

   Replace the placeholders, retaining the `s` suffix for an exact sector
   position. For the example above and partition 1, the command would be
   `resizepart 1 134219775s`. Confirm the shrink prompt only after verifying
   your calculation. Commands change the table immediately; `quit` does not
   undo them. If Parted proposes another boundary, do not accept a length
   smaller than the filesystem or any change to the start.
5. Verify the start is unchanged and
   `(end - start + 1) * logical sector bytes` is at least the reported filesystem
   size. Ensure the operating system sees the new layout before checking or
   mounting the filesystem, then return to the README's
   [final shrink checks](../README.md#shrink-a-filesystem).

GNU Parted documents the [partition-only `resizepart` operation][resizepart]
and the use of [sector units for exact positions][parted-units].

Windows CHKDSK's exFAT recognition requires the partition length to equal the
filesystem's `VolumeLength`. A successfully shrunk filesystem in a larger
partition may mount and remain readable while CHKDSK reports `RAW`. To check
the smaller filesystem with CHKDSK, first complete the separate partition resize
to the reported filesystem size. A `RAW` result alone does not establish this
size mismatch; verify the sizes and the successful resize result before changing
the partition.

For a regular image the same order applies: shrink the filesystem successfully,
then truncate the image. The CLI does not truncate images. On Windows,
`--shrink-partition` combines filesystem shrink and the partition update. A
cancelled shrink at `SOURCE_READY` leaves a clean filesystem at its original
size, so its backing storage must also retain that size. Never reduce backing
storage as a recovery step after an unsuccessful resize.

[parted-units]: https://www.gnu.org/s/parted/manual/parted.html#unit
