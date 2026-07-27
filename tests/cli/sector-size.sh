#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-sector-test.$$
image=$temporary/image.exfat

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

require_test_tools
format_exfat_image "$image" 8192 16384 4096
"$program" "$image"
check_exfat_image "$image"

echo "sector-size: passed"
