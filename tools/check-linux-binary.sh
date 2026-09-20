#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

if [ "$#" -ne 2 ]; then
	echo "usage: $0 BINARY ARCHITECTURE" >&2
	exit 2
fi

binary=$1
architecture=$2
case $architecture in
	x86_64) machine="Advanced Micro Devices X86-64" ;;
	arm64) machine=AArch64 ;;
	*)
		echo "unsupported Linux binary architecture: $architecture" >&2
		exit 1
		;;
esac

header=$(LC_ALL=C readelf -h "$binary")
elf_class=$(printf '%s\n' "$header" | sed -n 's/^ *Class: *//p')
elf_type=$(printf '%s\n' "$header" | sed -n 's/^ *Type: *\([^ ]*\).*/\1/p')
elf_machine=$(printf '%s\n' "$header" | sed -n 's/^ *Machine: *//p')
if [ "$elf_class" != ELF64 ] || [ "$elf_type" != DYN ] || [ "$elf_machine" != "$machine" ]; then
	echo "expected a 64-bit static PIE executable for $architecture" >&2
	printf '%s\n' "$header" >&2
	exit 1
fi

program_headers=$(LC_ALL=C readelf -l "$binary")
dynamic=$(LC_ALL=C readelf -d "$binary")
if printf '%s\n' "$program_headers" | grep -E '(^|[[:space:]])INTERP[[:space:]]' >/dev/null ||
	printf '%s\n' "$dynamic" | grep -F '(NEEDED)' >/dev/null; then
	echo "Linux binary must have no ELF interpreter or shared-library dependencies" >&2
	exit 1
fi

sections=$(LC_ALL=C readelf -S "$binary")
if printf '%s\n' "$sections" | grep -E '[[:space:]]\.symtab[[:space:]]' >/dev/null; then
	echo "Linux binary must be stripped" >&2
	exit 1
fi

echo "verified stripped $architecture static PIE executable: $binary"
