#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

if [ "$#" -ne 2 ]; then
	echo "usage: $0 SOURCE_DIRECTORY OUTPUT_DIRECTORY" >&2
	exit 2
fi

source_directory=$1
output_directory=$2
cmake_command=${CMAKE:-cmake}
deployment_target=11.0
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-macos-build.XXXXXX")

cleanup() {
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
	echo "macOS binary archives must be built on macOS ARM64" >&2
	exit 1
fi
if [ ! -d "$source_directory" ]; then
	echo "source directory does not exist: $source_directory" >&2
	exit 1
fi
source_directory=$(CDPATH= cd -- "$source_directory" && pwd)
if [ -e "$source_directory/.git" ]; then
	echo "source directory must be an extracted, Git-free source archive" >&2
	exit 1
fi
if [ ! -f "$source_directory/VERSION" ] || [ ! -f "$source_directory/.tarball-version" ]; then
	echo "source archive version metadata is missing" >&2
	exit 1
fi

package_version=$(sed -n '1p' "$source_directory/VERSION")
build_version=$(sed -n '1p' "$source_directory/.tarball-version")
if ! printf '%s\n' "$package_version" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' >/dev/null; then
	echo "invalid package version: $package_version" >&2
	exit 1
fi
if ! printf '%s\n' "$build_version" | grep -E '^[0-9A-Za-z][0-9A-Za-z.+-]*$' >/dev/null; then
	echo "invalid build version: $build_version" >&2
	exit 1
fi
case $build_version in
	"$package_version" | "$package_version"-*) ;;
	*)
		echo "package and build versions disagree" >&2
		exit 1
		;;
esac
if [ "${source_directory##*/}" != "exfat-resize-$build_version" ]; then
	echo "source directory and build versions disagree" >&2
	exit 1
fi

mkdir -p "$temporary/build" "$temporary/stage" "$output_directory"
"$cmake_command" -S "$source_directory" -B "$temporary/build" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
	-DEXFAT_RESIZE_BUILD_CLI=ON \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF
"$cmake_command" --build "$temporary/build" --parallel --target exfat-resize
"$cmake_command" --install "$temporary/build" --component Runtime \
	--prefix "$temporary/stage/usr/local" --strip

binary=$temporary/stage/usr/local/bin/exfat-resize
manual=$temporary/stage/usr/local/share/man/man8/exfat-resize.8
documentation=$temporary/stage/usr/local/share/doc/exfat_resize
contributing=$documentation/CONTRIBUTING.md
license=$documentation/LICENSE
readme=$documentation/README.md
transaction=$documentation/docs/TRANSACTION.md
if [ ! -x "$binary" ] || [ ! -f "$manual" ] || [ ! -f "$license" ] ||
	[ ! -f "$contributing" ] || [ ! -f "$readme" ] || [ ! -f "$transaction" ]; then
	echo "CMake runtime installation is incomplete" >&2
	exit 1
fi
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	echo "built CLI version does not match the source archive" >&2
	exit 1
fi

architectures=$(lipo -archs "$binary")
if [ "$architectures" != arm64 ]; then
	echo "unexpected Mach-O architecture set: $architectures" >&2
	exit 1
fi
build_info=$(xcrun vtool -show-build "$binary")
minimum_macos=$(printf '%s\n' "$build_info" | awk '$1 == "minos" { print $2; exit }')
if [ "$minimum_macos" != "$deployment_target" ]; then
	echo "unexpected minimum macOS version: $minimum_macos" >&2
	exit 1
fi
dependencies=$(LC_ALL=C otool -L "$binary" |
	sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
if [ "$dependencies" != /usr/lib/libSystem.B.dylib ]; then
	echo "unexpected dynamic-library dependency set:" >&2
	printf '%s\n' "$dependencies" >&2
	exit 1
fi
if ! codesign --verify --strict "$binary"; then
	echo "built CLI has an invalid ad-hoc signature" >&2
	exit 1
fi
signature=$(codesign -dvv "$binary" 2>&1)
if ! printf '%s\n' "$signature" | grep -Fx 'Signature=adhoc' >/dev/null; then
	echo "built CLI does not have an ad-hoc signature" >&2
	printf '%s\n' "$signature" >&2
	exit 1
fi

package=exfat-resize-$build_version-macos-arm64
package_directory=$temporary/$package
mkdir -p "$package_directory/docs"
install -m 0755 "$binary" "$package_directory/exfat-resize"
install -m 0644 "$manual" "$package_directory/exfat-resize.8"
install -m 0644 "$contributing" "$package_directory/CONTRIBUTING.md"
install -m 0644 "$license" "$package_directory/LICENSE"
install -m 0644 "$readme" "$package_directory/README.md"
install -m 0644 "$transaction" "$package_directory/docs/TRANSACTION.md"
install -m 0755 "$source_directory/packaging/linux/install.sh" "$package_directory/install.sh"
install -m 0755 "$source_directory/packaging/linux/uninstall.sh" \
	"$package_directory/uninstall.sh"

archive=$package.tar.gz
COPYFILE_DISABLE=1 tar -czf "$output_directory/$archive" -C "$temporary" "$package"

echo "built $output_directory/$archive"
echo "minimum macOS version: $minimum_macos"
