#!/bin/sh
# SPDX-License-Identifier: MIT

test_device=
test_mount=
test_sector_size=512

require_test_tools() {
	case $(uname -s) in
		Darwin)
			for tool in hdiutil newfs_exfat fsck_exfat; do
				command -v "$tool" >/dev/null || {
					echo "missing $tool" >&2
					exit 77
				}
			done
			;;
		Linux)
			# Administrative tools are not always in an unprivileged user's PATH.
			PATH=$PATH:/usr/local/sbin:/usr/sbin:/sbin
			export PATH
			for tool in mkfs.exfat fsck.exfat losetup mount.exfat-fuse sudo umount; do
				command -v "$tool" >/dev/null || {
					echo "missing $tool" >&2
					exit 77
				}
			done
			;;
		*)
			echo "unsupported test platform" >&2
			exit 77
			;;
	esac
}

raw_device() {
	case $1 in
		/dev/disk*) printf '/dev/r%s\n' "${1#/dev/}" ;;
		*) printf '%s\n' "$1" ;;
	esac
}

run_raw_device_command() {
	case $(uname -s) in
		Darwin) "$@" ;;
		Linux) sudo "$@" ;;
	esac
}

attach_image() {
	image=$1
	shift
	test_device=$(hdiutil attach "$@" \
	    -blocksize "$test_sector_size" \
	    -imagekey diskimage-class=CRawDiskImage "$image" |
	    awk 'NR == 1 { print $1 }')
}

attach_raw_image() {
	image=$1
	test_sector_size=${2:-512}
	case $(uname -s) in
		Darwin)
			attach_image "$image" -nomount
			;;
		Linux)
			test_device=$(sudo losetup --find --show \
			    --sector-size "$test_sector_size" "$image")
			;;
	esac
}

eject_image() {
	if [ -n "$test_device" ]; then
		case $(uname -s) in
			Darwin) diskutil eject "$test_device" >/dev/null ;;
			Linux) sudo losetup --detach "$test_device" ;;
		esac
		test_device=
	fi
}

format_exfat_image() {
	image=$1
	initial_sectors=$2
	backing_sectors=$3
	sector_size=${4:-512}
	cluster_size=${5:-}
	test_sector_size=$sector_size

	case $(uname -s) in
		Darwin)
			truncate -s "$((backing_sectors * sector_size))" "$image"
			attach_image "$image" -nomount
			if [ -n "$cluster_size" ]; then
				newfs_exfat -S "$sector_size" -b "$cluster_size" \
				    -s "$initial_sectors" "$(raw_device "$test_device")" >/dev/null
			else
				newfs_exfat -S "$sector_size" -s "$initial_sectors" \
				    "$(raw_device "$test_device")" >/dev/null
			fi
			eject_image
			;;
		Linux)
			truncate -s "$((initial_sectors * sector_size))" "$image"
			if [ "$sector_size" -eq 512 ]; then
				if [ -n "$cluster_size" ]; then
					mkfs.exfat -c "$cluster_size" "$image" >/dev/null
				else
					mkfs.exfat "$image" >/dev/null
				fi
			else
				test_device=$(sudo losetup --find --show \
				    --sector-size "$sector_size" "$image")
				if [ -n "$cluster_size" ]; then
					sudo mkfs.exfat -c "$cluster_size" "$test_device" >/dev/null
				else
					sudo mkfs.exfat "$test_device" >/dev/null
				fi
				eject_image
			fi
			truncate -s "$((backing_sectors * sector_size))" "$image"
			;;
	esac
}

mount_exfat_image() {
	image=$1
	mountpoint=$2

	case $(uname -s) in
		Darwin)
			attach_image "$image" -mountpoint "$mountpoint"
			;;
		Linux)
			test_device=$(sudo losetup --find --show "$image")
			sudo mount.exfat-fuse -o "uid=$(id -u),gid=$(id -g)" \
			    "$test_device" "$mountpoint"
			;;
	esac
	test_mount=$mountpoint
}

unmount_exfat_image() {
	if [ -z "$test_mount" ]; then
		return
	fi
	sync
	case $(uname -s) in
		Darwin) eject_image ;;
		Linux)
			sudo umount "$test_mount"
			sudo losetup --detach "$test_device"
			test_device=
			;;
	esac
	test_mount=
}

check_exfat_image() {
	image=$1

	case $(uname -s) in
		Darwin)
			attach_image "$image" -nomount
			fsck_exfat -n "$(raw_device "$test_device")"
			eject_image
			;;
		Linux)
			fsck.exfat -n "$image"
			;;
	esac
}

check_exfat_device() {
	case $(uname -s) in
		Darwin) fsck_exfat -n "$(raw_device "$test_device")" ;;
		Linux) sudo fsck.exfat -n "$test_device" ;;
	esac
}

cleanup_test_image() {
	unmount_exfat_image >/dev/null 2>&1 || true
	eject_image >/dev/null 2>&1 || true
}
