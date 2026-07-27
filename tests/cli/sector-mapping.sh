#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-sector-mapping-test.$$
large_sector_image=$temporary/large-sector.exfat
small_sector_image=$temporary/small-sector.exfat

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

require_test_tools

# A 4096-byte-sector filesystem must work through a 512-byte-sector device.
format_exfat_image "$large_sector_image" 8192 16384 4096
attach_raw_image "$large_sector_image" 512
device=$(raw_device "$test_device")
run_raw_device_command "$program" "$device"
eject_image
test_sector_size=4096
check_exfat_image "$large_sector_image"

# The inverse relationship cannot represent a filesystem sector as whole
# device sectors and must be rejected without modifying the image.
format_exfat_image "$small_sector_image" 65536 131072 512
checksum_before=$(cksum "$small_sector_image")
attach_raw_image "$small_sector_image" 4096
device=$(raw_device "$test_device")
if run_raw_device_command "$program" "$device" >"$temporary/rejected.out" 2>&1; then
	echo "filesystem sector smaller than device sector was accepted" >&2
	exit 1
fi
if ! grep -q "filesystem sector size is incompatible" "$temporary/rejected.out"; then
	echo "sector-size mismatch did not report its cause" >&2
	cat "$temporary/rejected.out" >&2
	exit 1
fi
eject_image
checksum_after=$(cksum "$small_sector_image")
if [ "$checksum_before" != "$checksum_after" ]; then
	echo "rejected sector-size mismatch modified the image" >&2
	exit 1
fi
test_sector_size=512
check_exfat_image "$small_sector_image"

echo "sector-mapping: passed"
