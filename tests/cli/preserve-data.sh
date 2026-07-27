#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=${TMPDIR:-/tmp}/exfat-resize-data-test.$$
image=$temporary/image.exfat
mountpoint=$temporary/mount
initial_sectors=${INITIAL_SECTORS:-65536}
backing_sectors=${BACKING_SECTORS:-4194304}
cluster_size=${CLUSTER_SIZE:-}

. "$(dirname "$0")/lib.sh"

write_manifest() {
	root=$1
	output=$2

	(
		cd "$root"
		# Ignore AppleDouble metadata created by macOS for the test files.
		LC_ALL=C find . ! -name '._*' -print | LC_ALL=C sort |
		    while IFS= read -r path; do
			    if [ -d "$path" ]; then
				    printf 'directory\t%s\n' "$path"
			    elif [ -f "$path" ]; then
				    set -- $(cksum <"$path")
				    printf 'file\t%s\t%s\t%s\n' "$path" "$1" "$2"
			    else
				    printf 'other\t%s\n' "$path"
			    fi
		    done
	) >"$output"
}

cleanup() {
	cleanup_test_image
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$mountpoint"

require_test_tools
format_exfat_image "$image" "$initial_sectors" "$backing_sectors" 512 "$cluster_size"
set -- $(od -An -tu4 -j 88 -N 4 "$image")
old_cluster_heap=$1
set -- $(od -An -tu1 -j 109 -N 1 "$image")
sectors_per_cluster=$((1 << $1))
mount_exfat_image "$image" "$mountpoint"
mkdir -p "$mountpoint/documents/archive"
mkdir -p "$mountpoint/documents/empty directory"
printf '%s\n' "exFAT resize data preservation test" >"$mountpoint/documents/readme.txt"
dd if=/dev/urandom of="$mountpoint/documents/archive/payload.bin" \
    bs=1048576 count=28 2>/dev/null
: >"$mountpoint/documents/empty.txt"
printf '%s\n' "unicode filename" >"$mountpoint/documents/archive/résumé-東京.txt"
long_name=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.txt
printf '%s\n' "long filename" >"$mountpoint/documents/archive/$long_name"
write_manifest "$mountpoint/documents" "$temporary/before.manifest"
unmount_exfat_image

"$program" "$image"
set -- $(od -An -tu4 -j 88 -N 4 "$image")
new_cluster_heap=$1
heap_shift_clusters=$(((new_cluster_heap - old_cluster_heap) / sectors_per_cluster))
minimum_heap_shift=${MIN_HEAP_SHIFT_CLUSTERS:-1}
if [ "$heap_shift_clusters" -lt "$minimum_heap_shift" ]; then
	echo "cluster heap moved by only $heap_shift_clusters clusters; expected at least $minimum_heap_shift" >&2
	exit 1
fi
check_exfat_image "$image"

mount_exfat_image "$image" "$mountpoint"
write_manifest "$mountpoint/documents" "$temporary/after.manifest"
diff -u "$temporary/before.manifest" "$temporary/after.manifest"
unmount_exfat_image

echo "preserve-data: passed"
