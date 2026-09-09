#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-size-test.$$
image=$temporary/image.exfat
kibibyte_image=$temporary/kibibyte.exfat
mebibyte_image=$temporary/mebibyte.exfat

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

expect_invalid_size() {
	size=$1
	expect_failure "$program" "$temporary/missing.exfat" "$size"
	if ! grep -Fq "exfat-resize: invalid size: $size" "$temporary/rejected.out"; then
		echo "size was not rejected by the parser: $size" >&2
		return 1
	fi
}

expect_valid_size_syntax() {
	size=$1
	expect_failure "$program" "$temporary/missing.exfat" "$size"
	if grep -Fq "exfat-resize: invalid size:" "$temporary/rejected.out"; then
		echo "valid size syntax was rejected: $size" >&2
		return 1
	fi
}

expected_resize_output() {
	printf '%s\n' \
		"exfat-resize: checking filesystem" \
		"exfat-resize: preparing resize" \
		"exfat-resize: resizing filesystem" \
		"exfat-resize: finalizing resize" \
		"exfat-resize: resized $1 to $2 bytes"
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
if [ "$output" != "$(expected_resize_output "$image" "$((target_sectors * sector_size))")" ]; then
	echo "unexpected resize output: $output" >&2
	exit 1
fi
if [ "$(read_volume_sector_count "$image")" -ne "$target_sectors" ]; then
	echo "explicit byte size produced the wrong filesystem length" >&2
	exit 1
fi

check_exfat_image "$image"
expect_failure "$program" "$image" "$target_bytes"
expect_failure "$program" "$image" "$((target_sectors * sector_size - 1))"
expect_failure "$program" "$image" "$((backing_bytes + sector_size))"
expect_failure "$program" "$image" 18446744073709551615
check_exfat_image "$image"

format_exfat_image "$kibibyte_image" 65536 131072
output=$("$program" "$kibibyte_image" 49152K)
if [ "$output" != "$(expected_resize_output "$kibibyte_image" "$((49152 * 1024))")" ]; then
	echo "unexpected K-suffix resize output: $output" >&2
	exit 1
fi
if [ "$(read_volume_sector_count "$kibibyte_image")" -ne 98304 ]; then
	echo "K-suffix size produced the wrong filesystem length" >&2
	exit 1
fi
check_exfat_image "$kibibyte_image"

format_exfat_image "$mebibyte_image" 65536 131072
output=$("$program" "$mebibyte_image" 48M)
if [ "$output" != "$(expected_resize_output "$mebibyte_image" "$((48 * 1024 * 1024))")" ]; then
	echo "unexpected M-suffix resize output: $output" >&2
	exit 1
fi
if [ "$(read_volume_sector_count "$mebibyte_image")" -ne 98304 ]; then
	echo "M-suffix size produced the wrong filesystem length" >&2
	exit 1
fi
check_exfat_image "$mebibyte_image"

for size in 18446744073709551615 1K 1M 1G 18014398509481983K 17592186044415M \
	17179869183G; do
	expect_valid_size_syntax "$size"
done
for size in 0 0K K not-a-number 1k 1m 1g 1T 1KB 1KiB 1.5G 1GG +1G \
	18446744073709551616 18014398509481984K 17592186044416M 17179869184G; do
	expect_invalid_size "$size"
done

echo "explicit-size: passed"
