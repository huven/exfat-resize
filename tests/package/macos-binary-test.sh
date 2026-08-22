#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	printf '%s\n' \
		"usage: $0 [--developer-id-team TEAM_ID] MACOS_ARCHIVE SOURCE_DIRECTORY"
}

fail() {
	echo "macos-binary-test: $*" >&2
	exit 1
}

developer_id_team=
while [ "$#" -gt 0 ]; do
	case $1 in
		--developer-id-team)
			[ "$#" -ge 2 ] || fail "--developer-id-team requires a value"
			developer_id_team=$2
			shift 2
			;;
		--)
			shift
			break
			;;
		-*) fail "unknown option: $1" ;;
		*) break ;;
	esac
done

if [ "$#" -ne 2 ]; then
	usage >&2
	exit 2
fi
if [ -n "$developer_id_team" ] &&
	! printf '%s\n' "$developer_id_team" | grep -E '^[A-Z0-9]{10}$' >/dev/null; then
	fail "invalid Developer ID team: $developer_id_team"
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

expected_entries=$(
	printf '%s\n' \
		"$package/" \
		"$package/CONTRIBUTING.md" \
		"$package/LICENSE" \
		"$package/README.md" \
		"$package/docs/" \
		"$package/docs/PARTITIONING.md" \
		"$package/docs/TRANSACTION.md" \
		"$package/exfat-resize" \
		"$package/exfat-resize.8" \
		"$package/install.sh" \
		"$package/uninstall.sh" |
		sort
)
actual_entries=$(LC_ALL=C tar -tf "$archive" | sort)
if [ "$actual_entries" != "$expected_entries" ]; then
	fail "macOS archive contains an unexpected entry set"
fi
unexpected_types=$(LC_ALL=C tar -tvf "$archive" |
	awk 'substr($1, 1, 1) != "-" && substr($1, 1, 1) != "d" { print }')
if [ -n "$unexpected_types" ]; then
	echo "macOS archive contains an unexpected entry type:" >&2
	printf '%s\n' "$unexpected_types" >&2
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
	printf '%s\n' CONTRIBUTING.md LICENSE README.md docs exfat-resize exfat-resize.8 \
		install.sh uninstall.sh |
		sort
)
actual=$(find "$package_directory" -mindepth 1 -maxdepth 1 -exec basename {} \; | sort)
if [ "$actual" != "$expected" ]; then
	echo "macOS archive contains an unexpected file set" >&2
	printf 'expected:\n%s\nactual:\n%s\n' "$expected" "$actual" >&2
	exit 1
fi
expected_documentation=$(printf '%s\n' PARTITIONING.md TRANSACTION.md | sort)
documentation=$(find "$package_directory/docs" -mindepth 1 -maxdepth 1 -exec basename {} \; | sort)
if [ "$documentation" != "$expected_documentation" ]; then
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
	[ "$(stat -f '%Lp' "$package_directory/CONTRIBUTING.md")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/LICENSE")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/README.md")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/docs/PARTITIONING.md")" != 644 ] ||
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
cmp "$source_directory/CONTRIBUTING.md" "$package_directory/CONTRIBUTING.md"
cmp "$source_directory/LICENSE" "$package_directory/LICENSE"
cmp "$source_directory/README.md" "$package_directory/README.md"
cmp "$source_directory/docs/PARTITIONING.md" "$package_directory/docs/PARTITIONING.md"
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
	echo "archived CLI has an invalid signature" >&2
	exit 1
fi
signature=$(codesign -dvvv "$binary" 2>&1)
identifier=$(printf '%s\n' "$signature" | sed -n 's/^Identifier=//p')
if [ "$identifier" != exfat-resize ]; then
	fail "CLI has an unexpected code-signing identifier: $identifier"
fi
entitlements=$(codesign -d --entitlements - "$binary" 2>/dev/null)
if [ -n "$entitlements" ]; then
	fail "CLI has unexpected code-signing entitlements"
fi
if [ -z "$developer_id_team" ]; then
	if ! printf '%s\n' "$signature" | grep -Fx 'Signature=adhoc' >/dev/null; then
		echo "archived CLI does not have an ad-hoc signature" >&2
		printf '%s\n' "$signature" >&2
		exit 1
	fi
	signature_description=ad-hoc
else
	authority=$(printf '%s\n' "$signature" | sed -n 's/^Authority=//p' | sed -n '1p')
	team_identifier=$(printf '%s\n' "$signature" | sed -n 's/^TeamIdentifier=//p')
	case $authority in
		"Developer ID Application: "*) ;;
		*) fail "CLI was not signed with a Developer ID Application certificate" ;;
	esac
	if [ "$team_identifier" != "$developer_id_team" ]; then
		fail "unexpected Developer ID team: $team_identifier"
	fi
	if ! printf '%s\n' "$signature" | grep -E '^Timestamp=.' >/dev/null; then
		fail "Developer ID signature does not have a secure timestamp"
	fi
	if ! printf '%s\n' "$signature" | grep -F 'flags=0x10000(runtime)' >/dev/null; then
		fail "Developer ID signature does not have only the hardened-runtime flag"
	fi
	if ! codesign -vvvv -R=notarized --check-notarization "$binary"; then
		fail "CLI does not satisfy Apple's notarized code requirement"
	fi
	signature_description="Developer ID team $team_identifier"
fi

DESTDIR=$temporary/install-root "$package_directory/install.sh"
install_prefix=$temporary/install-root/usr/local
installed_binary=$install_prefix/bin/exfat-resize
installed_manual=$install_prefix/share/man/man8/exfat-resize.8
installed_contributing=$install_prefix/share/doc/exfat-resize/CONTRIBUTING.md
installed_license=$install_prefix/share/doc/exfat-resize/LICENSE
installed_readme=$install_prefix/share/doc/exfat-resize/README.md
installed_partitioning=$install_prefix/share/doc/exfat-resize/docs/PARTITIONING.md
installed_transaction=$install_prefix/share/doc/exfat-resize/docs/TRANSACTION.md
if [ "$("$installed_binary" --version)" != "exfat-resize $build_version" ] ||
	[ ! -f "$installed_manual" ] || [ ! -f "$installed_license" ] ||
	[ ! -f "$installed_contributing" ] || [ ! -f "$installed_readme" ] ||
	[ ! -f "$installed_partitioning" ] || [ ! -f "$installed_transaction" ]; then
	echo "installed macOS archive is incomplete" >&2
	exit 1
fi
if [ "$(find "$install_prefix" -type f | wc -l)" -ne 7 ]; then
	echo "installer created an unexpected file set" >&2
	exit 1
fi
if ! codesign --verify --strict "$installed_binary"; then
	fail "installed CLI has an invalid signature"
fi
if [ -n "$developer_id_team" ] &&
	! codesign -vvvv -R=notarized --check-notarization "$installed_binary"; then
	fail "installed CLI does not satisfy Apple's notarized code requirement"
fi

touch "$install_prefix/bin/unrelated" "$install_prefix/share/man/man8/unrelated.8" \
	"$install_prefix/share/doc/exfat-resize/unrelated" \
	"$install_prefix/share/doc/exfat-resize/docs/unrelated"
DESTDIR=$temporary/install-root "$package_directory/uninstall.sh"
if [ -e "$installed_binary" ] || [ -e "$installed_manual" ] || [ -e "$installed_license" ] ||
	[ -e "$installed_contributing" ] || [ -e "$installed_readme" ] ||
	[ -e "$installed_partitioning" ] || [ -e "$installed_transaction" ]; then
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
echo "signature: $signature_description"
