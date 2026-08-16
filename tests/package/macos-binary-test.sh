#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

if [ "$#" -ne 2 ]; then
	echo "usage: $0 MACOS_ARCHIVE SOURCE_DIRECTORY" >&2
	exit 2
fi

archive=$1
source_directory=$2
archive_directory=$(CDPATH= cd -- "$(dirname "$archive")" && pwd)
archive_name=${archive##*/}
archive=$archive_directory/$archive_name
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-macos-test.XXXXXX")

cleanup() {
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM

case $archive_name in
	exfat-resize-*-macos-arm64.tar.gz) ;;
	*)
		echo "unexpected macOS archive name: $archive_name" >&2
		exit 1
		;;
esac

build_version=$(sed -n '1p' "$source_directory/.tarball-version")
package=exfat-resize-$build_version-macos-arm64
if [ "$archive_name" != "$package.tar.gz" ]; then
	echo "macOS archive and source build versions disagree" >&2
	exit 1
fi

mkdir -p "$temporary/extracted" "$temporary/install-root"
tar -xzf "$archive" -C "$temporary/extracted"
package_directory=$temporary/extracted/$package
top_level=$(find "$temporary/extracted" -mindepth 1 -maxdepth 1 -exec basename {} \;)
if [ "$top_level" != "$package" ] || [ ! -d "$package_directory" ]; then
	echo "macOS archive has an unexpected top-level directory" >&2
	exit 1
fi
expected=$(
	printf '%s\n' LICENSE README.md docs exfat-resize exfat-resize.8 install.sh uninstall.sh |
		sort
)
actual=$(find "$package_directory" -mindepth 1 -maxdepth 1 -exec basename {} \; | sort)
if [ "$actual" != "$expected" ]; then
	echo "macOS archive contains an unexpected file set" >&2
	printf 'expected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
	exit 1
fi
documentation=$(find "$package_directory/docs" -mindepth 1 -maxdepth 1 -exec basename {} \;)
if [ "$documentation" != TRANSACTION.md ]; then
	echo "macOS archive contains an unexpected documentation file set" >&2
	exit 1
fi
if [ "$(stat -f '%Lp' "$package_directory/exfat-resize")" != 755 ] ||
	[ "$(stat -f '%Lp' "$package_directory/install.sh")" != 755 ] ||
	[ "$(stat -f '%Lp' "$package_directory/uninstall.sh")" != 755 ]; then
	echo "macOS archive executable modes are incorrect" >&2
	exit 1
fi
if [ "$(stat -f '%Lp' "$package_directory/exfat-resize.8")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/LICENSE")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/README.md")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/docs/TRANSACTION.md")" != 644 ]; then
	echo "macOS archive data-file modes are incorrect" >&2
	exit 1
fi

binary=$package_directory/exfat-resize
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	echo "archived CLI version is incorrect" >&2
	exit 1
fi
if ! grep -F "exfat-resize $build_version" "$package_directory/exfat-resize.8" >/dev/null; then
	echo "archived manual page has an incorrect version" >&2
	exit 1
fi
cmp "$source_directory/LICENSE" "$package_directory/LICENSE"
cmp "$source_directory/README.md" "$package_directory/README.md"
cmp "$source_directory/docs/TRANSACTION.md" "$package_directory/docs/TRANSACTION.md"

architectures=$(lipo -archs "$binary")
if [ "$architectures" != arm64 ]; then
	echo "archived CLI has an unexpected Mach-O architecture set: $architectures" >&2
	exit 1
fi
build_info=$(xcrun vtool -show-build "$binary")
minimum_macos=$(printf '%s\n' "$build_info" | awk '$1 == "minos" { print $2; exit }')
if [ "$minimum_macos" != 11.0 ]; then
	echo "archived CLI has an unexpected minimum macOS version: $minimum_macos" >&2
	exit 1
fi
dependencies=$(LC_ALL=C otool -L "$binary" |
	sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
if [ "$dependencies" != /usr/lib/libSystem.B.dylib ]; then
	echo "archived CLI has an unexpected dynamic-library dependency set:" >&2
	printf '%s\n' "$dependencies" >&2
	exit 1
fi
if ! codesign --verify --strict "$binary"; then
	echo "archived CLI has an invalid ad-hoc signature" >&2
	exit 1
fi
signature=$(codesign -dvv "$binary" 2>&1)
if ! printf '%s\n' "$signature" | grep -Fx 'Signature=adhoc' >/dev/null; then
	echo "archived CLI does not have an ad-hoc signature" >&2
	printf '%s\n' "$signature" >&2
	exit 1
fi

DESTDIR=$temporary/install-root "$package_directory/install.sh"
install_prefix=$temporary/install-root/usr/local
installed_binary=$install_prefix/bin/exfat-resize
installed_manual=$install_prefix/share/man/man8/exfat-resize.8
installed_license=$install_prefix/share/doc/exfat-resize/LICENSE
installed_readme=$install_prefix/share/doc/exfat-resize/README.md
installed_transaction=$install_prefix/share/doc/exfat-resize/docs/TRANSACTION.md
if [ "$("$installed_binary" --version)" != "exfat-resize $build_version" ] ||
	[ ! -f "$installed_manual" ] || [ ! -f "$installed_license" ] ||
	[ ! -f "$installed_readme" ] || [ ! -f "$installed_transaction" ]; then
	echo "installed macOS archive is incomplete" >&2
	exit 1
fi
if [ "$(find "$install_prefix" -type f | wc -l)" -ne 5 ]; then
	echo "installer created an unexpected file set" >&2
	exit 1
fi

touch "$install_prefix/bin/unrelated" "$install_prefix/share/man/man8/unrelated.8" \
	"$install_prefix/share/doc/exfat-resize/unrelated" \
	"$install_prefix/share/doc/exfat-resize/docs/unrelated"
DESTDIR=$temporary/install-root "$package_directory/uninstall.sh"
if [ -e "$installed_binary" ] || [ -e "$installed_manual" ] || [ -e "$installed_license" ] ||
	[ -e "$installed_readme" ] || [ -e "$installed_transaction" ]; then
	echo "uninstaller left an installed project file behind" >&2
	exit 1
fi
for unrelated in "$install_prefix/bin/unrelated" "$install_prefix/share/man/man8/unrelated.8" \
	"$install_prefix/share/doc/exfat-resize/unrelated" \
	"$install_prefix/share/doc/exfat-resize/docs/unrelated"; do
	if [ ! -f "$unrelated" ]; then
		echo "uninstaller removed an unrelated file: $unrelated" >&2
		exit 1
	fi
done

"$source_directory/tests/cli/grow-image.sh" "$binary"

echo "macOS binary archive test: passed"
echo "minimum macOS version: $minimum_macos"
