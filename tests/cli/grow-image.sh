#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-test.$$
image=$temporary/image.exfat
producer_status_file=$temporary/producer.status

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

require_test_tools
format_exfat_image "$image" 65536 131072
# Closing a progress-output pipe must not terminate an active transaction.
(
	set +e
	"$program" "$image"
	printf '%s\n' "$?" >"$producer_status_file"
) | head -n 2
if [ ! -f "$producer_status_file" ]; then
	echo "progress-pipe producer did not report its status" >&2
	exit 1
fi
producer_status=$(cat "$producer_status_file")
if [ "$producer_status" -ne 0 ]; then
	echo "progress-pipe producer returned $producer_status, expected 0" >&2
	exit 1
fi
check_exfat_image "$image"

echo "grow-image: passed"
