#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-size-test.$$
image=$temporary/image.exfat

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

expect_failure() {
	if "$@" >"$temporary/rejected.out" 2>&1; then
		echo "command unexpectedly succeeded: $*" >&2
		return 1
	fi
}

read_volume_sector_count() {
	set -- $(od -An -tu1 -j 72 -N 8 "$1")
	if [ "$#" -ne 8 ]; then
		echo "could not read exFAT VolumeLength" >&2
		return 1
	fi
	printf '%s\n' "$(($1 + ($2 << 8) + ($3 << 16) + ($4 << 24) + \
	    ($5 << 32) + ($6 << 40) + ($7 << 48) + ($8 << 56)))"
}

require_test_tools
format_exfat_image "$image" 65536 131072

target_sectors=98305
backing_sectors=131072
sector_size=512
target_bytes=$((target_sectors * sector_size + 127))
backing_bytes=$((backing_sectors * sector_size))

output=$("$program" "$image" "$target_bytes")
if [ "$output" != "exfat-resize: resized $image" ]; then
	echo "unexpected resize output: $output" >&2
	exit 1
fi
if [ "$(read_volume_sector_count "$image")" -ne "$target_sectors" ]; then
	echo "explicit byte size produced the wrong filesystem length" >&2
	exit 1
fi

check_exfat_image "$image"
expect_failure "$program" "$image" 0
expect_failure "$program" "$image" not-a-number
expect_failure "$program" "$image" "$target_bytes"
expect_failure "$program" "$image" "$((target_sectors * sector_size - 1))"
expect_failure "$program" "$image" "$((backing_bytes + sector_size))"
expect_failure "$program" "$image" 18446744073709551615
check_exfat_image "$image"

echo "explicit-size: passed"
