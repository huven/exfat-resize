#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

project_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
dist_build=build/dist
dist_output=dist

cd "$project_root"
commit=$(git rev-parse --verify "HEAD^{commit}")
version_info=$(sh tools/version.sh --commit "$project_root" "$commit")
package_version=$(printf '%s\n' "$version_info" | sed -n '1p')
build_version=$(printf '%s\n' "$version_info" | sed -n '2p')

rm -rf "$dist_build"
mkdir -p "$dist_build" "$dist_output"
printf '%s\n' "$package_version" >"$dist_build/package-version"
printf '%s\n' "$build_version" >"$dist_build/build-version"

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
