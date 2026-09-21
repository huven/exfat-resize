#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

if [ "$#" -ne 3 ]; then
	echo "usage: $0 SOURCE_DIRECTORY OUTPUT_DIRECTORY MUSL_LICENSE" >&2
	exit 2
fi

source_directory=$1
output_directory=$2
musl_license=$3
cmake_command=${CMAKE:-cmake}
compiler=${CC:-cc}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-linux-build.XXXXXX")

cleanup() {
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM

if [ ! -s "$musl_license" ]; then
	echo "musl copyright notice is missing or empty: $musl_license" >&2
	exit 1
fi

if [ "$(uname -s)" != Linux ]; then
	echo "Linux binary archives must be built on Linux" >&2
	exit 1
fi
case $(uname -m) in
	x86_64)
		architecture=x86_64
		musl_architecture=x86_64
		;;
	aarch64 | arm64)
		architecture=arm64
		musl_architecture=aarch64
		;;
	*)
		echo "Linux binary archives require an x86_64 or ARM64 builder" >&2
		exit 1
		;;
esac
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

# Check the compiler's libc before building: static glibc would also satisfy the
# final ELF checks, but has different portability and distribution requirements.
cat > "$temporary/libc-probe.c" <<'EOF'
#include <features.h>
#ifdef __GLIBC__
#error Linux binary archives require a musl compiler
#endif
int main(void) { return 0; }
EOF
"$compiler" "$temporary/libc-probe.c" -o "$temporary/libc-probe"
probe_headers=$(LC_ALL=C readelf -l "$temporary/libc-probe")
if ! printf '%s\n' "$probe_headers" |
	grep -F "/ld-musl-$musl_architecture.so.1]" >/dev/null; then
	echo "Linux binary archives require a native musl compiler" >&2
	exit 1
fi

mkdir -p "$temporary/build" "$temporary/stage" "$output_directory"
"$cmake_command" -S "$source_directory" -B "$temporary/build" -DCMAKE_BUILD_TYPE=Release \
	"-DCMAKE_C_COMPILER=$compiler" -DCMAKE_C_FLAGS=-fPIE -DCMAKE_EXE_LINKER_FLAGS=-static-pie \
	-DEXFAT_RESIZE_BUILD_CLI=ON -DEXFAT_RESIZE_BUILD_TESTS=OFF
"$cmake_command" --build "$temporary/build" --parallel --target exfat-resize
"$cmake_command" --install "$temporary/build" --component Runtime \
	--prefix "$temporary/stage/usr/local" --strip

binary=$temporary/stage/usr/local/bin/exfat-resize
manual=$temporary/stage/usr/local/share/man/man8/exfat-resize.8
documentation=$temporary/stage/usr/local/share/doc/exfat_resize
contributing=$documentation/CONTRIBUTING.md
license=$documentation/LICENSE
readme=$documentation/README.md
library_reference=$documentation/docs/LIBRARY.md
partitioning=$documentation/docs/PARTITIONING.md
transaction=$documentation/docs/TRANSACTION.md
if [ ! -x "$binary" ] || [ ! -f "$manual" ] || [ ! -f "$license" ] ||
	[ ! -f "$contributing" ] || [ ! -f "$readme" ] || [ ! -f "$library_reference" ] ||
	[ ! -f "$partitioning" ] || [ ! -f "$transaction" ]; then
	echo "CMake runtime installation is incomplete" >&2
	exit 1
fi
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	echo "built CLI version does not match the source archive" >&2
	exit 1
fi

"$source_directory/tools/check-linux-binary.sh" "$binary" "$architecture"

package=exfat-resize-$build_version-linux-$architecture
package_directory=$temporary/$package
mkdir -p "$package_directory/docs"
install -m 0755 "$binary" "$package_directory/exfat-resize"
install -m 0644 "$manual" "$package_directory/exfat-resize.8"
install -m 0644 "$contributing" "$package_directory/CONTRIBUTING.md"
install -m 0644 "$license" "$package_directory/LICENSE"
install -m 0644 "$musl_license" "$package_directory/LICENSE-musl"
install -m 0644 "$readme" "$package_directory/README.md"
install -m 0644 "$library_reference" "$package_directory/docs/LIBRARY.md"
install -m 0644 "$partitioning" "$package_directory/docs/PARTITIONING.md"
install -m 0644 "$transaction" "$package_directory/docs/TRANSACTION.md"
install -m 0755 "$source_directory/packaging/linux/install.sh" "$package_directory/install.sh"
install -m 0755 "$source_directory/packaging/linux/uninstall.sh" "$package_directory/uninstall.sh"

archive=$package.tar.gz
tar -czf "$output_directory/$archive" -C "$temporary" "$package"

echo "built $output_directory/$archive"
