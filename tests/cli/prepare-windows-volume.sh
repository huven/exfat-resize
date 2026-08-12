#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

output=${1:?output directory is required}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-windows-volume.XXXXXX")
raw=$temporary/prepared.raw
vhdx=$temporary/prepared.vhdx
mountpoint=$temporary/mount
loop_device=
test_mount=

cleanup() {
	if [ -n "$test_mount" ]; then
		sudo umount "$test_mount" >/dev/null 2>&1 || true
		test_mount=
	fi
	if [ -n "$loop_device" ]; then
		sudo losetup --detach "$loop_device" >/dev/null 2>&1 || true
		loop_device=
	fi
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

for tool in blockdev fsck.exfat losetup mkfs.exfat mount.exfat-fuse qemu-img sgdisk; do
	command -v "$tool" >/dev/null || {
		echo "missing $tool" >&2
		exit 1
	}
done

total_sectors=393216
partition_start=2048
initial_sectors=196608
initial_end=$((partition_start + initial_sectors - 1))
initial_bytes=$((initial_sectors * 512))

mkdir -p "$mountpoint"
truncate -s "$((total_sectors * 512))" "$raw"
sgdisk --clear \
	--new="1:${partition_start}:${initial_end}" \
	--typecode=1:0700 \
	--change-name=1:EXRTEST "$raw" >/dev/null

attach_raw_disk() {
	loop_device=$(sudo losetup --find --show --partscan "$raw")
	partition=${loop_device}p1
	index=0
	while [ ! -b "$partition" ] && [ "$index" -lt 50 ]; do
		sleep 0.1
		index=$((index + 1))
	done
	if [ ! -b "$partition" ]; then
		echo "partition device did not appear: $partition" >&2
		exit 1
	fi
}

detach_raw_disk() {
	sudo losetup --detach "$loop_device"
	loop_device=
}

attach_raw_disk
sudo mkfs.exfat -c 32768 -L EXRTEST "$partition" >/dev/null
sudo mount.exfat-fuse -o "uid=$(id -u),gid=$(id -g)" "$partition" "$mountpoint"
test_mount=$mountpoint

dd if=/dev/urandom of="$mountpoint/payload.bin" bs=1M count=8 status=none
mkdir "$mountpoint/small-files"
index=1
while [ "$index" -le 200 ]; do
	test_file=$mountpoint/small-files/$index.txt
	printf 'exfat-resize Windows volume fixture %d\n' "$index" >"$test_file"
	index=$((index + 1))
done
payload_hash=$(sha256sum "$mountpoint/payload.bin" | awk '{ print $1 }')
sync
sudo umount "$mountpoint"
test_mount=
sudo fsck.exfat -n "$partition"
actual_bytes=$(sudo blockdev --getsize64 "$partition")
if [ "$actual_bytes" -ne "$initial_bytes" ]; then
	echo "wrong initial partition size: $actual_bytes, expected $initial_bytes" >&2
	exit 1
fi
sudo mount.exfat-fuse -o "uid=$(id -u),gid=$(id -g),ro" "$partition" "$mountpoint"
test_mount=$mountpoint
actual_hash=$(sha256sum "$mountpoint/payload.bin" | awk '{ print $1 }')
if [ "$actual_hash" != "$payload_hash" ]; then
	echo "payload changed while preparing the fixture" >&2
	exit 1
fi
sudo umount "$mountpoint"
test_mount=
detach_raw_disk

qemu-img convert -f raw -O vhdx -o subformat=dynamic "$raw" "$vhdx"
qemu-img check -f vhdx "$vhdx"

mkdir -p "$output"
cp "$vhdx" "$output/prepared.vhdx"
printf '%s\n' "$payload_hash" >"$output/payload.sha256"
printf 'prepare-windows-volume: passed\n'
