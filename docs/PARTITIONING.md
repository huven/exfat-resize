# Partition resizing outside exfat-resize

`exfat-resize` is focused on resizing exFAT filesystems correctly, not on
general disk partitioning. Windows provides two explicit exceptions:
[`--grow-partition`](../README.md#growing-the-containing-partition) enlarges a
basic GPT or MBR partition into immediately trailing unallocated space;
[`--shrink-partition`](../README.md#shrinking-the-containing-partition) reduces
its end after successful filesystem shrink. Otherwise `exfat-resize` never
changes a partition table.

When the containing partition is too small, enlarge it first with a suitable
partitioning tool, then run `exfat-resize`. The following are starting points,
not a partitioning guide. Make a verified backup, keep the exFAT filesystem
unmounted, never move its start sector, and never shrink it during growth or
failure recovery.

## growpart

[`growpart`][growpart] extends one partition to the end of the disk or the
beginning of the next partition. It supports a dry run:

```text
sudo growpart --dry-run /dev/sdX PARTITION_NUMBER
sudo growpart /dev/sdX PARTITION_NUMBER
```

Verify the resulting partition layout before running `exfat-resize`.

## GParted Live

[GParted Live][gparted-live] is available for x86-64 computers. GParted does not
support growing exFAT filesystems. Identify the whole target disk carefully and
use the included terminal to enlarge the partition:

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

Windows CHKDSK's exFAT recognition requires the partition length to equal the
filesystem's `VolumeLength`. A successfully shrunk filesystem in a larger
partition may mount and remain readable while CHKDSK reports `RAW`. To check
the smaller filesystem with CHKDSK, first complete the separate partition resize
to the reported filesystem size. A `RAW` result alone does not establish this
size mismatch; verify the sizes and the successful resize result before changing
the partition.

For a regular image the same order applies: shrink the filesystem successfully,
then truncate the image. The CLI does not truncate images. On Windows,
`--shrink-partition` combines filesystem shrink and the partition update. A cancelled shrink at `SOURCE_READY` leaves a clean filesystem at its
original size, so its backing storage must also retain that size. Never reduce
backing storage as a recovery step after an unsuccessful resize.
