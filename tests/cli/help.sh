#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
help=$("$program" --help)

require_text() {
	case $help in
		*"$1"*) ;;
	*)
		echo "help output does not contain: $1" >&2
		exit 1
		;;
	esac
}

require_text "Usage: exfat-resize DEVICE [SIZE]"
require_text "Arguments:"
require_text "Desired filesystem size in bytes or with an optional"
require_text "K, M, or G suffix (powers of 1024)"
require_text "Options:"
require_text "Safety:"
require_text "Documentation:"
require_text "Make and verify a backup"
require_text "Read the safety requirements"
require_text "README.md distributed with exfat-resize"
require_text "https://github.com/huven/exfat-resize#safety"

echo "help: passed"
