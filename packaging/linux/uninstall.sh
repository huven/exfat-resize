#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

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
rm -f "$install_root/bin/exfat-resize" "$install_root/share/man/man8/exfat-resize.8" \
	"$install_root/share/doc/exfat-resize/LICENSE" \
	"$install_root/share/doc/exfat-resize/README.md" \
	"$install_root/share/doc/exfat-resize/docs/TRANSACTION.md"
rmdir "$install_root/share/doc/exfat-resize/docs" 2>/dev/null || true
rmdir "$install_root/share/doc/exfat-resize" 2>/dev/null || true

echo "removed exfat-resize from $install_root"
