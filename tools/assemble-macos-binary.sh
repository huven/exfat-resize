#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	echo "usage: $0 UNSIGNED_ARCHIVE SIGNED_EXECUTABLE OUTPUT_DIRECTORY"
}

fail() {
	echo "assemble-macos-binary: $*" >&2
	exit 1
}

if [ "$#" -ne 3 ]; then
	usage >&2
	exit 2
fi
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
	fail "assembly requires macOS ARM64"
fi

archive_input=$1
archive_directory=$(CDPATH= cd -- "$(dirname "$archive_input")" && pwd)
archive_name=${archive_input##*/}
archive=$archive_directory/$archive_name
[ -f "$archive" ] || fail "unsigned archive does not exist: $archive"

case $archive_name in
	exfat-resize-*-macos-arm64.tar.gz) package=${archive_name%.tar.gz} ;;
	*) fail "unexpected unsigned archive name: $archive_name" ;;
esac

signed_input=$2
signed_directory=$(CDPATH= cd -- "$(dirname "$signed_input")" && pwd)
signed_name=${signed_input##*/}
signed_binary=$signed_directory/$signed_name
[ -f "$signed_binary" ] || fail "signed executable does not exist: $signed_binary"
[ "$signed_name" = "$package.signed" ] || fail "unexpected signed executable name: $signed_name"

output_directory_input=$3
mkdir -p "$output_directory_input"
output_directory=$(CDPATH= cd -- "$output_directory_input" && pwd)
output_archive=$output_directory/$archive_name
[ "$output_archive" != "$archive" ] || fail "output would overwrite the unsigned input"
[ ! -e "$output_archive" ] || fail "output archive already exists: $output_archive"

temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-macos-assemble.XXXXXX")
cleanup() {
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

mkdir "$temporary/extracted"
COPYFILE_DISABLE=1 tar -xzf "$archive" -C "$temporary/extracted"
package_directory=$temporary/extracted/$package
unsigned_binary=$package_directory/exfat-resize
if [ ! -d "$package_directory" ] || [ ! -f "$unsigned_binary" ]; then
	fail "unsigned archive extraction is incomplete"
fi
codesign --verify --strict "$unsigned_binary" || fail "unsigned CLI signature is invalid"
codesign --verify --strict "$signed_binary" || fail "signed CLI signature is invalid"

unsigned_canonical=$temporary/unsigned-canonical
signed_canonical=$temporary/signed-canonical
install -m 0755 "$unsigned_binary" "$unsigned_canonical"
install -m 0755 "$signed_binary" "$signed_canonical"
codesign --remove-signature "$unsigned_canonical"
codesign --remove-signature "$signed_canonical"
codesign --force --identifier exfat-resize --options runtime --sign - "$unsigned_canonical"
codesign --force --identifier exfat-resize --options runtime --sign - "$signed_canonical"
if ! cmp "$unsigned_canonical" "$signed_canonical"; then
	fail "signed CLI does not match the tested unsigned CLI"
fi

install -m 0755 "$signed_binary" "$unsigned_binary"
candidate_archive=$temporary/$archive_name
COPYFILE_DISABLE=1 tar -czf "$candidate_archive" -C "$temporary/extracted" "$package"
install -m 0644 "$candidate_archive" "$output_archive"

output_sha256=$(shasum -a 256 "$output_archive" | awk '{ print $1 }')
printf 'assembled signed macOS archive: %s\n' "$output_archive"
printf 'output archive SHA-256: %s\n' "$output_sha256"
