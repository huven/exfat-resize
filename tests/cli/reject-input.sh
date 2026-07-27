#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-reject-test.$$
plain_image=$temporary/plain.img
corrupt_image=$temporary/corrupt.exfat

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

require_test_tools
truncate -s 64M "$plain_image"
expect_failure "$program" "$plain_image"

format_exfat_image "$corrupt_image" 65536 131072
dd if=/dev/zero of="$corrupt_image" bs=512 count=1 seek=0 conv=notrunc 2>/dev/null
dd if=/dev/zero of="$corrupt_image" bs=512 count=1 seek=12 conv=notrunc 2>/dev/null
expect_failure "$program" "$corrupt_image"

echo "reject-input: passed"
