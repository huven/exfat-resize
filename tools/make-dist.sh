#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

host_cc=${HOSTCC:-cc}
host_cflags=${HOSTCFLAGS:--O2}
dist_build=build/dist
dist_output=dist
source_tree=$dist_build/source
commit=$(git rev-parse --verify "HEAD^{commit}")

rm -rf "$dist_build"
mkdir -p "$source_tree" "$dist_output"
git -c tar.umask=022 archive --format=tar "$commit" >"$dist_build/source.tar"
tar -xf "$dist_build/source.tar" -C "$source_tree"

# HOSTCC and HOSTCFLAGS conventionally contain shell-separated argument lists.
# Globbing is disabled above so their intentional expansion cannot match paths.
# shellcheck disable=SC2086
$host_cc $host_cflags -I"$source_tree/src" \
	-o "$dist_build/print-version" "$source_tree/tools/print-version.c"
version=$("$dist_build/print-version")
case $version in
	""|*[!0-9A-Za-z.-]*)
		echo "invalid version: $version" >&2
		exit 1
		;;
esac

package=exfat-resize-$version
archive=$package.tar.gz
git -c tar.umask=022 archive --format=tar --prefix="$package/" "$commit" \
	>"$dist_build/$package.tar"
gzip -n -9 -c <"$dist_build/$package.tar" >"$dist_build/$archive"
mv "$dist_build/$archive" "$dist_output/$archive"

if command -v sha256sum >/dev/null 2>&1; then
	(cd "$dist_output" && sha256sum "$archive" > "$archive.sha256")
else
	(cd "$dist_output" && shasum -a 256 "$archive" > "$archive.sha256")
fi

echo "created $dist_output/$archive"
echo "created $dist_output/$archive.sha256"
