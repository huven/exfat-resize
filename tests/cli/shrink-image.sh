#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

program=$1
metadata_program=${DIRECTORY_METADATA_TEST:?directory metadata test program is required}
temporary=${TMPDIR:-/tmp}/exfat-resize-shrink-test.$$
image=$temporary/image.exfat
mountpoint=$temporary/mount

. "$(dirname "$0")/lib.sh"

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$mountpoint"

manifest() {
	(
		cd "$mountpoint"
		LC_ALL=C find documents -type f | LC_ALL=C sort |
		    while IFS= read -r path; do
			    printf '%s\t' "$path"
			    cksum <"$path"
		    done
	) >"$1"
}

require_test_tools
format_exfat_image "$image" 131072 131072 "${FILESYSTEM_SECTOR_SIZE:-512}" 65536
mount_exfat_image "$image" "$mountpoint"
# Force the retained payload into the tail, then create free destinations below it.
dd if=/dev/zero of="$mountpoint/filler.bin" bs=1048576 count=40 2>/dev/null
mkdir -p "$mountpoint/documents/archive" "$mountpoint/documents/empty directory"
dd if=/dev/urandom of="$mountpoint/documents/archive/payload.bin" \
    bs=1048576 count=6 2>/dev/null
printf '%s\n' 'shrink preserves metadata and contents' >"$mountpoint/documents/résumé-東京.txt"
: >"$mountpoint/documents/empty.txt"
rm "$mountpoint/filler.bin"
unmount_exfat_image
mount_exfat_image "$image" "$mountpoint" ro
manifest "$temporary/before.manifest"
unmount_exfat_image
"$metadata_program" "$image" >"$temporary/before.metadata"

"$program" "$image" 32M
# Truncate only after a successful filesystem resize.
truncate -s 32M "$image"
check_exfat_image "$image"
"$metadata_program" "$image" >"$temporary/shrunk.metadata"
diff -u "$temporary/before.metadata" "$temporary/shrunk.metadata"
mount_exfat_image "$image" "$mountpoint" ro
manifest "$temporary/shrunk.manifest"
diff -u "$temporary/before.manifest" "$temporary/shrunk.manifest"
unmount_exfat_image

truncate -s 64M "$image"
"$program" "$image"
check_exfat_image "$image"
"$metadata_program" "$image" >"$temporary/grown.metadata"
diff -u "$temporary/before.metadata" "$temporary/grown.metadata"
mount_exfat_image "$image" "$mountpoint" ro
manifest "$temporary/grown.manifest"
diff -u "$temporary/before.manifest" "$temporary/grown.manifest"
unmount_exfat_image

echo "shrink-image: passed"
