#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

archive_directory=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
prefix=${PREFIX:-/usr/local}
destdir=${DESTDIR:-}

case $prefix in
	/*) ;;
	*)
		echo "PREFIX must be an absolute path: $prefix" >&2
		exit 1
		;;
esac
case $destdir in
	"" | /*) ;;
	*)
		echo "DESTDIR must be empty or an absolute path: $destdir" >&2
		exit 1
		;;
esac

install_root=$destdir$prefix
mkdir -p "$install_root/bin" "$install_root/share/man/man8" "$install_root/share/doc/exfat-resize"
mkdir -p "$install_root/share/doc/exfat-resize/docs"
install -m 0755 "$archive_directory/exfat-resize" "$install_root/bin/exfat-resize"
install -m 0644 "$archive_directory/exfat-resize.8" "$install_root/share/man/man8/exfat-resize.8"
install -m 0644 "$archive_directory/CONTRIBUTING.md" \
	"$install_root/share/doc/exfat-resize/CONTRIBUTING.md"
install -m 0644 "$archive_directory/LICENSE" "$install_root/share/doc/exfat-resize/LICENSE"
install -m 0644 "$archive_directory/README.md" "$install_root/share/doc/exfat-resize/README.md"
install -m 0644 "$archive_directory/docs/LIBRARY.md" \
	"$install_root/share/doc/exfat-resize/docs/LIBRARY.md"
install -m 0644 "$archive_directory/docs/PARTITIONING.md" \
	"$install_root/share/doc/exfat-resize/docs/PARTITIONING.md"
install -m 0644 "$archive_directory/docs/TRANSACTION.md" \
	"$install_root/share/doc/exfat-resize/docs/TRANSACTION.md"

echo "installed exfat-resize under $install_root"
