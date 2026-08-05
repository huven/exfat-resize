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
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-linux-build.XXXXXX")

cleanup() {
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM

if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
	echo "Linux binary archives must be built on Linux x86_64" >&2
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
"$cmake_command" -S "$source_directory" -B "$temporary/build" -DCMAKE_BUILD_TYPE=Release \
	-DEXFAT_RESIZE_BUILD_CLI=ON -DEXFAT_RESIZE_BUILD_TESTS=OFF
"$cmake_command" --build "$temporary/build" --parallel --target exfat-resize
"$cmake_command" --install "$temporary/build" --component Runtime \
	--prefix "$temporary/stage/usr/local" --strip

binary=$temporary/stage/usr/local/bin/exfat-resize
manual=$temporary/stage/usr/local/share/man/man8/exfat-resize.8
license=$temporary/stage/usr/local/share/doc/exfat_resize/LICENSE
if [ ! -x "$binary" ] || [ ! -f "$manual" ] || [ ! -f "$license" ]; then
	echo "CMake runtime installation is incomplete" >&2
	exit 1
fi
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	echo "built CLI version does not match the source archive" >&2
	exit 1
fi

needed=$(LC_ALL=C readelf -d "$binary" |
	sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
if [ "$needed" != libc.so.6 ]; then
	echo "unexpected shared-library dependency set:" >&2
	printf '%s\n' "$needed" >&2
	exit 1
fi
interpreter=$(LC_ALL=C readelf -l "$binary" |
	sed -n 's/.*Requesting program interpreter: \([^]]*\)\].*/\1/p')
if [ "$interpreter" != /lib64/ld-linux-x86-64.so.2 ]; then
	echo "unexpected ELF interpreter: $interpreter" >&2
	exit 1
fi
glibc_versions=$(LC_ALL=C readelf --version-info "$binary" |
	grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu)
maximum_glibc=$(printf '%s\n' "$glibc_versions" | tail -n 1)
newest_glibc=$(printf '%s\n%s\n' GLIBC_2.28 "$maximum_glibc" | sort -Vu | tail -n 1)
if [ "$newest_glibc" != GLIBC_2.28 ]; then
	echo "binary requires a glibc symbol newer than GLIBC_2.28:" >&2
	printf '%s\n' "$glibc_versions" >&2
	exit 1
fi

package=exfat-resize-$build_version-linux-x86_64-glibc
package_directory=$temporary/$package
mkdir "$package_directory"
install -m 0755 "$binary" "$package_directory/exfat-resize"
install -m 0644 "$manual" "$package_directory/exfat-resize.8"
install -m 0644 "$license" "$package_directory/LICENSE"
install -m 0755 "$source_directory/packaging/linux/install.sh" "$package_directory/install.sh"
install -m 0755 "$source_directory/packaging/linux/uninstall.sh" "$package_directory/uninstall.sh"

archive=$package.tar.gz
tar -czf "$output_directory/$archive" -C "$temporary" "$package"
(
	cd "$output_directory"
	sha256sum "$archive" >"$archive.sha256"
)

echo "built $output_directory/$archive"
echo "maximum referenced glibc symbol: $maximum_glibc"
