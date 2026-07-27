#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-flags-test.$$
image=$temporary/image.exfat

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

set_flags() {
	flag=$1
	printf "\\$flag\\000" |
	    dd of="$image" bs=1 seek=106 conv=notrunc 2>/dev/null
}

expect_unchanged_rejection() {
	expected=$1
	before=$(cksum "$image")
	if "$program" "$image" >"$temporary/rejected.out" 2>&1; then
		echo "unsafe volume unexpectedly resized" >&2
		exit 1
	fi
	if ! grep -F "$expected" "$temporary/rejected.out" >/dev/null; then
		echo "missing rejection reason: $expected" >&2
		exit 1
	fi
	after=$(cksum "$image")
	if [ "$before" != "$after" ]; then
		echo "rejected resize changed the image" >&2
		exit 1
	fi
}

require_test_tools
format_exfat_image "$image" 65536 131072

set_flags 002
expect_unchanged_rejection "filesystem is marked dirty"

set_flags 004
expect_unchanged_rejection "filesystem has the media-failure flag set"

set_flags 010
"$program" "$image"
set -- $(od -An -tu1 -j 106 -N 2 "$image")
if [ "$1" -ne 0 ] || [ "$2" -ne 0 ]; then
	echo "successful resize left volume flags set: $1 $2" >&2
	exit 1
fi
check_exfat_image "$image"

echo "volume-flags: passed"
