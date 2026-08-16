#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	printf '%s\n' \
		"usage: $0 --check UNSIGNED_ARCHIVE" \
		"       $0 --identity IDENTITY --notary-profile PROFILE \\" \
		"           UNSIGNED_ARCHIVE OUTPUT_DIRECTORY"
}

fail() {
	echo "sign-macos-binary: $*" >&2
	exit 1
}

check_only=0
identity=
notary_profile=
while [ "$#" -gt 0 ]; do
	case $1 in
		--check)
			check_only=1
			shift
			;;
		--identity)
			[ "$#" -ge 2 ] || fail "--identity requires a value"
			identity=$2
			shift 2
			;;
		--notary-profile)
			[ "$#" -ge 2 ] || fail "--notary-profile requires a value"
			notary_profile=$2
			shift 2
			;;
		-h | --help)
			usage
			exit 0
			;;
		--)
			shift
			break
			;;
		-*) fail "unknown option: $1" ;;
		*) break ;;
	esac
done

if [ "$check_only" -eq 1 ]; then
	if [ "$#" -ne 1 ] || [ -n "$identity" ] || [ -n "$notary_profile" ]; then
		usage >&2
		exit 2
	fi
else
	if [ "$#" -ne 2 ] || [ -z "$identity" ] || [ -z "$notary_profile" ]; then
		usage >&2
		exit 2
	fi
fi

if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
	fail "signing requires macOS ARM64"
fi

input_archive=$1
archive_directory=$(CDPATH= cd -- "$(dirname "$input_archive")" && pwd)
input_name=${input_archive##*/}
archive=$archive_directory/$input_name
[ -f "$archive" ] || fail "archive does not exist: $archive"

case $input_name in
	exfat-resize-*-macos-arm64.tar.gz)
		package=${input_name%.tar.gz}
		gzip -t "$archive"
		content_sha256=$(gzip -dc "$archive" | shasum -a 256 | awk '{ print $1 }')
		;;
	exfat-resize-*-macos-arm64.tar)
		package=${input_name%.tar}
		content_sha256=$(shasum -a 256 "$archive" | awk '{ print $1 }')
		;;
	*) fail "unexpected unsigned archive name: $input_name" ;;
esac

tool_root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
package_version=$(sed -n '1p' "$tool_root/VERSION")
build_version=${package#exfat-resize-}
build_version=${build_version%-macos-arm64}
if ! printf '%s\n' "$package_version" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' >/dev/null; then
	fail "invalid package version: $package_version"
fi
if ! printf '%s\n' "$build_version" | grep -E '^[0-9A-Za-z][0-9A-Za-z.+-]*$' >/dev/null; then
	fail "invalid build version: $build_version"
fi
case $build_version in
	"$package_version" | "$package_version"-*) ;;
	*) fail "archive and checkout package versions disagree" ;;
esac

input_sha256=$(shasum -a 256 "$archive" | awk '{ print $1 }')
printf 'input archive SHA-256: %s\n' "$input_sha256"
printf 'uncompressed tar SHA-256: %s\n' "$content_sha256"

expected_entries=$(
	printf '%s\n' \
		"$package/" \
		"$package/LICENSE" \
		"$package/README.md" \
		"$package/docs/" \
		"$package/docs/TRANSACTION.md" \
		"$package/exfat-resize" \
		"$package/exfat-resize.8" \
		"$package/install.sh" \
		"$package/uninstall.sh" |
		sort
)
actual_entries=$(LC_ALL=C tar -tf "$archive" | sort)
if [ "$actual_entries" != "$expected_entries" ]; then
	fail "archive contains an unexpected file set"
fi
unexpected_types=$(LC_ALL=C tar -tvf "$archive" |
	awk 'substr($1, 1, 1) != "-" && substr($1, 1, 1) != "d" { print }')
if [ -n "$unexpected_types" ]; then
	echo "archive contains an unexpected entry type:" >&2
	printf '%s\n' "$unexpected_types" >&2
	exit 1
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-macos-sign.XXXXXX")
cleanup() {
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

mkdir "$temporary/extracted"
COPYFILE_DISABLE=1 tar -xf "$archive" -C "$temporary/extracted"
package_directory=$temporary/extracted/$package
binary=$package_directory/exfat-resize
if [ ! -d "$package_directory" ] || [ ! -x "$binary" ]; then
	fail "archive extraction is incomplete"
fi
if [ "$(stat -f '%Lp' "$binary")" != 755 ] ||
	[ "$(stat -f '%Lp' "$package_directory/install.sh")" != 755 ] ||
	[ "$(stat -f '%Lp' "$package_directory/uninstall.sh")" != 755 ]; then
	fail "archive executable modes are incorrect"
fi
if [ "$(stat -f '%Lp' "$package_directory/exfat-resize.8")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/LICENSE")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/README.md")" != 644 ] ||
	[ "$(stat -f '%Lp' "$package_directory/docs/TRANSACTION.md")" != 644 ]; then
	fail "archive data-file modes are incorrect"
fi

cmp "$tool_root/LICENSE" "$package_directory/LICENSE"
cmp "$tool_root/README.md" "$package_directory/README.md"
cmp "$tool_root/docs/TRANSACTION.md" "$package_directory/docs/TRANSACTION.md"
cmp "$tool_root/packaging/linux/install.sh" "$package_directory/install.sh"
cmp "$tool_root/packaging/linux/uninstall.sh" "$package_directory/uninstall.sh"

if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	fail "archived CLI version is incorrect"
fi
"$binary" --help >/dev/null
architectures=$(lipo -archs "$binary")
[ "$architectures" = arm64 ] || fail "unexpected Mach-O architecture set: $architectures"
build_info=$(xcrun vtool -show-build "$binary")
minimum_macos=$(printf '%s\n' "$build_info" | awk '$1 == "minos" { print $2; exit }')
[ "$minimum_macos" = 11.0 ] || fail "unexpected minimum macOS version: $minimum_macos"
dependencies=$(LC_ALL=C otool -L "$binary" |
	sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
if [ "$dependencies" != /usr/lib/libSystem.B.dylib ]; then
	echo "unexpected dynamic-library dependency set:" >&2
	printf '%s\n' "$dependencies" >&2
	exit 1
fi
codesign --verify --strict "$binary" || fail "archived CLI has an invalid ad-hoc signature"
signature=$(codesign -dvv "$binary" 2>&1)
if ! printf '%s\n' "$signature" | grep -Fx 'Signature=adhoc' >/dev/null; then
	fail "archived CLI does not have the expected ad-hoc signature"
fi

printf 'validated unsigned archive: %s\n' "$input_name"
printf 'build version: %s\n' "$build_version"
printf 'minimum macOS version: %s\n' "$minimum_macos"
if [ "$check_only" -eq 1 ]; then
	exit 0
fi

output_directory_input=$2
mkdir -p "$output_directory_input"
output_directory=$(CDPATH= cd -- "$output_directory_input" && pwd)
output_name=$package.tar.gz
output_archive=$output_directory/$output_name
output_log=$output_directory/$output_name.notarization.json
[ "$output_archive" != "$archive" ] || fail "output would overwrite the unsigned input"
[ ! -e "$output_archive" ] || fail "output archive already exists: $output_archive"
[ ! -e "$output_log" ] || fail "notarization log already exists: $output_log"

identities=$(security find-identity -v -p codesigning)
if ! printf '%s\n' "$identities" | grep -F "$identity" >/dev/null; then
	echo "$identities" >&2
	fail "code-signing identity is not available: $identity"
fi

codesign --force --options runtime --timestamp --sign "$identity" "$binary"
codesign --verify --strict --verbose=2 "$binary"
signing_details=$(codesign -dvvv "$binary" 2>&1)
authority=$(printf '%s\n' "$signing_details" | sed -n 's/^Authority=//p' | sed -n '1p')
team_identifier=$(printf '%s\n' "$signing_details" | sed -n 's/^TeamIdentifier=//p')
case $authority in
	"Developer ID Application: "*) ;;
	*) fail "CLI was not signed with a Developer ID Application certificate" ;;
esac
if ! printf '%s\n' "$team_identifier" | grep -E '^[A-Z0-9]{10}$' >/dev/null; then
	fail "signed CLI has an invalid Team Identifier: $team_identifier"
fi
if ! printf '%s\n' "$signing_details" | grep -E '^Timestamp=.' >/dev/null; then
	fail "signed CLI does not have a secure timestamp"
fi
if ! printf '%s\n' "$signing_details" | grep -E '^CodeDirectory .*\(.*runtime.*\)' >/dev/null; then
	fail "signed CLI does not enable the hardened runtime"
fi
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	fail "signed CLI version is incorrect"
fi

notary_archive=$temporary/$package-notarization.zip
COPYFILE_DISABLE=1 ditto -c -k --keepParent "$package_directory" "$notary_archive"
submission=$temporary/notarization-submission.json
notary_exit=0
xcrun notarytool submit "$notary_archive" \
	--keychain-profile "$notary_profile" --wait --output-format json >"$submission" ||
	notary_exit=$?
cat "$submission"
submission_id=$(plutil -extract id raw -o - "$submission" 2>/dev/null || true)
notary_status=$(plutil -extract status raw -o - "$submission" 2>/dev/null || true)
notary_log=$temporary/notarization-log.json
if [ -n "$submission_id" ] && [ -n "$notary_status" ]; then
	if ! xcrun notarytool log --keychain-profile "$notary_profile" \
		"$submission_id" "$notary_log"; then
		fail "could not retrieve notarization log for $submission_id"
	fi
	cat "$notary_log"
fi
[ "$notary_exit" -eq 0 ] || fail "notarization submission failed"
[ -n "$submission_id" ] || fail "notarization response did not contain a submission ID"
[ "$notary_status" = Accepted ] || fail "notarization status is $notary_status"

candidate_archive=$temporary/$output_name
COPYFILE_DISABLE=1 tar -czf "$candidate_archive" -C "$temporary/extracted" "$package"

mkdir "$temporary/final-check"
COPYFILE_DISABLE=1 tar -xzf "$candidate_archive" -C "$temporary/final-check"
final_binary=$temporary/final-check/$package/exfat-resize
codesign --verify --strict --verbose=2 "$final_binary"
if [ "$("$final_binary" --version)" != "exfat-resize $build_version" ]; then
	fail "final archived CLI version is incorrect"
fi
notarization_attempt=1
while ! notarization_check=$(codesign -vvvv -R=notarized \
	--check-notarization "$final_binary" 2>&1); do
	if [ "$notarization_attempt" -ge 6 ]; then
		printf '%s\n' "$notarization_check" >&2
		fail "notarization ticket is unavailable for the final CLI"
	fi
	notarization_attempt=$((notarization_attempt + 1))
	sleep 5
done
printf '%s\n' "$notarization_check"

install -m 0644 "$candidate_archive" "$output_archive"
install -m 0644 "$notary_log" "$output_log"
output_sha256=$(shasum -a 256 "$output_archive" | awk '{ print $1 }')
printf 'built signed and notarized archive: %s\n' "$output_archive"
printf 'notarization log: %s\n' "$output_log"
printf 'Developer ID authority: %s\n' "$authority"
printf 'Team Identifier: %s\n' "$team_identifier"
printf 'output archive SHA-256: %s\n' "$output_sha256"
