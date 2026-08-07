#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

if [ "$#" -ne 2 ]; then
	echo "usage: $0 LINUX_ARCHIVE SOURCE_DIRECTORY" >&2
	exit 2
fi

archive=$1
source_directory=$2
archive_directory=$(CDPATH= cd -- "$(dirname "$archive")" && pwd)
archive_name=${archive##*/}
archive=$archive_directory/$archive_name
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-linux-test.XXXXXX")

cleanup() {
	rm -rf "$temporary"
}

trap cleanup EXIT HUP INT TERM

case $archive_name in
	exfat-resize-*-linux-x86_64-glibc.tar.gz) ;;
	*)
		echo "unexpected Linux archive name: $archive_name" >&2
		exit 1
		;;
esac
if [ ! -f "$archive.sha256" ]; then
	echo "Linux archive checksum is missing" >&2
	exit 1
fi
(
	cd "$archive_directory"
	sha256sum --check "$archive_name.sha256"
)

build_version=$(sed -n '1p' "$source_directory/.tarball-version")
package=exfat-resize-$build_version-linux-x86_64-glibc
if [ "$archive_name" != "$package.tar.gz" ]; then
	echo "Linux archive and source build versions disagree" >&2
	exit 1
fi

mkdir -p "$temporary/extracted" "$temporary/install-root"
tar -xzf "$archive" -C "$temporary/extracted"
package_directory=$temporary/extracted/$package
top_level=$(find "$temporary/extracted" -mindepth 1 -maxdepth 1 -printf '%f\n')
if [ "$top_level" != "$package" ] || [ ! -d "$package_directory" ]; then
	echo "Linux archive has an unexpected top-level directory" >&2
	exit 1
fi
expected=$(
	printf '%s\n' LICENSE README.md docs exfat-resize exfat-resize.8 install.sh uninstall.sh |
		sort
)
actual=$(find "$package_directory" -mindepth 1 -maxdepth 1 -printf '%f\n' | sort)
if [ "$actual" != "$expected" ]; then
	echo "Linux archive contains an unexpected file set" >&2
	printf 'expected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
	exit 1
fi
documentation=$(find "$package_directory/docs" -mindepth 1 -maxdepth 1 -printf '%f\n')
if [ "$documentation" != TRANSACTION.md ]; then
	echo "Linux archive contains an unexpected documentation file set" >&2
	exit 1
fi
if [ "$(stat -c '%a' "$package_directory/exfat-resize")" != 755 ] ||
	[ "$(stat -c '%a' "$package_directory/install.sh")" != 755 ] ||
	[ "$(stat -c '%a' "$package_directory/uninstall.sh")" != 755 ]; then
	echo "Linux archive executable modes are incorrect" >&2
	exit 1
fi
if [ "$(stat -c '%a' "$package_directory/exfat-resize.8")" != 644 ] ||
	[ "$(stat -c '%a' "$package_directory/LICENSE")" != 644 ] ||
	[ "$(stat -c '%a' "$package_directory/README.md")" != 644 ] ||
	[ "$(stat -c '%a' "$package_directory/docs/TRANSACTION.md")" != 644 ]; then
	echo "Linux archive data-file modes are incorrect" >&2
	exit 1
fi
if [ "$("$package_directory/exfat-resize" --version)" != "exfat-resize $build_version" ]; then
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
	echo "installed Linux archive is incomplete" >&2
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

echo "Linux binary archive test: passed"
