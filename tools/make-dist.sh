#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

project_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dist_build=build/dist
dist_output=dist
cmake_command=${CMAKE:-cmake}

cd "$project_root"
commit=$(git rev-parse --verify "HEAD^{commit}")

rm -rf "$dist_build"
mkdir -p "$dist_build" "$dist_output"
"$cmake_command" \
	"-DEXFAT_RESIZE_SOURCE_DIR=$project_root" \
	"-DEXFAT_RESIZE_COMMIT=$commit" \
	"-DEXFAT_RESIZE_VERSION_OUTPUT_DIR=$dist_build" \
	-P "$project_root/tools/version.cmake"
build_version=$(cat "$dist_build/build-version")

package=exfat-resize-$build_version
archive=$package.tar.gz
git -c tar.umask=022 archive --format=tar --prefix="$package/" \
	--add-virtual-file="$package/.tarball-version:$build_version" "$commit" \
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
