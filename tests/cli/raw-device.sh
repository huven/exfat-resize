#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-device-test.$$
image=$temporary/image.exfat

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

require_test_tools
format_exfat_image "$image" 65536 131072

mkdir "$temporary/mount"
mount_exfat_image "$image" "$temporary/mount"
device=$(raw_device "$test_device")
if run_raw_device_command "$program" "$device" >"$temporary/mounted.out" 2>&1; then
	echo "mounted raw device was accepted" >&2
	exit 1
fi
if ! grep -q "mounted or already in use" "$temporary/mounted.out"; then
	echo "mounted raw device did not report that it was in use" >&2
	cat "$temporary/mounted.out" >&2
	exit 1
fi
unmount_exfat_image

attach_raw_image "$image"
device=$(raw_device "$test_device")
run_raw_device_command "${DEVICE_LOCK_TEST:?}" "$device"
run_raw_device_command "$program" "$device"
check_exfat_device
eject_image

echo "raw-device: passed"
