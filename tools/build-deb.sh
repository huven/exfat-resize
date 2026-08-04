#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
LC_ALL=C
export LC_ALL

if [ "$#" -ne 2 ]; then
	echo "usage: $0 SOURCE_ARCHIVE OUTPUT_DIRECTORY" >&2
	exit 2
fi

archive=$1
output_directory=$2
cmake_command=${CMAKE:-cmake}
cpack_command=${CPACK:-cpack}

case $archive in
	/*) ;;
	*) archive=$(pwd)/$archive ;;
esac
case $output_directory in
	/*) ;;
	*) output_directory=$(pwd)/$output_directory ;;
esac

if [ ! -f "$archive" ]; then
	echo "source archive not found: $archive" >&2
	exit 1
fi

for tool in dpkg dpkg-buildflags dpkg-deb gzip sha256sum tar; do
	command -v "$tool" >/dev/null 2>&1 || {
		echo "required Debian package tool not found: $tool" >&2
		exit 1
	}
done

if [ "$(dpkg --print-architecture)" != amd64 ]; then
	echo "the initial Debian package must be built natively on amd64" >&2
	exit 1
fi

mkdir -p "$output_directory"
if find "$output_directory" -mindepth 1 -maxdepth 1 -print -quit |
	grep . >/dev/null; then
	echo "output directory is not empty: $output_directory" >&2
	exit 1
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-deb.XXXXXX")
cleanup() {
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

archive_root=$(tar -tzf "$archive" | sed -n '1{s,/.*,,;p;}')
case $archive_root in
	exfat-resize-*) ;;
	*)
		echo "source archive has an unexpected root: $archive_root" >&2
		exit 1
		;;
esac
if ! tar -tzf "$archive" |
	awk -F/ -v root="$archive_root" '$1 != root { exit 1 }'; then
	echo "source archive contains files outside $archive_root" >&2
	exit 1
fi

tar -xzf "$archive" -C "$temporary"
source_directory=$temporary/$archive_root
package_version=$(sed -n '1p' "$source_directory/VERSION")
build_version=$(sed -n '1p' "$source_directory/.tarball-version")

case $package_version in
	[0-9]*.[0-9]*.[0-9]*) ;;
	*)
		echo "invalid package version in source archive: $package_version" >&2
		exit 1
		;;
esac
if [ -z "$build_version" ]; then
	echo "source archive has an empty build version" >&2
	exit 1
fi

export DEB_BUILD_MAINT_OPTIONS=hardening=+all
c_flags="$(dpkg-buildflags --get CPPFLAGS) $(dpkg-buildflags --get CFLAGS)"
linker_flags=$(dpkg-buildflags --get LDFLAGS)

"$cmake_command" -S "$source_directory" -B "$temporary/build" \
	-DCMAKE_BUILD_TYPE=None \
	-DCMAKE_C_FLAGS="$c_flags" \
	-DCMAKE_EXE_LINKER_FLAGS="$linker_flags" \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_DOCDIR=share/doc/exfat-resize \
	-DEXFAT_RESIZE_BUILD_CLI=ON \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF \
	-DEXFAT_RESIZE_BUILD_DEB=ON
"$cmake_command" --build "$temporary/build" --parallel
"$cpack_command" --config "$temporary/build/CPackConfig.cmake" \
	-G DEB -B "$output_directory"

if [ "$(find "$output_directory" -maxdepth 1 -type f -name '*.deb' | wc -l | tr -d ' ')" -ne 2 ]; then
	echo "CPack produced an unexpected Debian package set" >&2
	exit 1
fi

runtime_package=
development_package=
deb_version=
architecture=
for package in "$output_directory"/*.deb; do
	package_name=$(dpkg-deb -f "$package" Package)
	case $package_name in
		exfat-resize)
			if [ -n "$runtime_package" ]; then
				echo "CPack produced more than one runtime package" >&2
				exit 1
			fi
			runtime_package=$package
			;;
		libexfat-resize-dev)
			if [ -n "$development_package" ]; then
				echo "CPack produced more than one development package" >&2
				exit 1
			fi
			development_package=$package
			;;
		*)
			echo "CPack produced an unexpected package: $package_name" >&2
			exit 1
			;;
	esac

	current_version=$(dpkg-deb -f "$package" Version)
	current_architecture=$(dpkg-deb -f "$package" Architecture)
	if [ -z "$deb_version" ]; then
		deb_version=$current_version
		architecture=$current_architecture
	elif [ "$current_version" != "$deb_version" ] ||
		[ "$current_architecture" != "$architecture" ]; then
		echo "Debian packages disagree on version or architecture" >&2
		exit 1
	fi
done
if [ -z "$runtime_package" ] || [ -z "$development_package" ]; then
	echo "CPack did not produce both expected Debian packages" >&2
	exit 1
fi
case $deb_version in
	"$package_version"-*) package_release=${deb_version#"$package_version"-} ;;
	*)
		echo "Debian package version disagrees with source: $deb_version" >&2
		exit 1
		;;
esac
if [ -z "$package_release" ]; then
	echo "Debian package has an empty package release" >&2
	exit 1
fi
if [ "$architecture" != amd64 ]; then
	echo "Debian package has an unexpected architecture: $architecture" >&2
	exit 1
fi

runtime_filename=${runtime_package##*/}
development_filename=${development_package##*/}

(
	cd "$output_directory"
	sha256sum "$runtime_filename" "$development_filename" |
		sort -k2 >DEB_SHA256SUMS
	printf '%s\n' \
		"package_version=$package_version" \
		"build_version=$build_version" \
		"package_release=$package_release" \
		"architecture=$architecture" \
		"runtime_filename=$runtime_filename" \
		"development_filename=$development_filename" \
		>DEB_PACKAGE_METADATA
)

printf 'built Debian packages for %s-%s (%s, %s)\n' \
	"$package_version" "$package_release" "$build_version" "$architecture"
