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
	exfat-resize-*-macos-arm64.tar.gz) package=${input_name%.tar.gz} ;;
	*) fail "unexpected unsigned archive name: $input_name" ;;
esac
gzip -t "$archive"

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

binary_entry=$package/exfat-resize
binary_entry_count=$(LC_ALL=C tar -tf "$archive" |
	awk -v expected="$binary_entry" '$0 == expected { count++ } END { print count + 0 }')
[ "$binary_entry_count" -eq 1 ] || fail "archive must contain exactly one $binary_entry"
binary_type=$(LC_ALL=C tar -tvf "$archive" "$binary_entry" |
	awk 'NR == 1 { print substr($1, 1, 1) }')
[ "$binary_type" = - ] || fail "archived CLI is not a regular file"

temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-macos-sign.XXXXXX")
cleanup() {
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

binary=$temporary/exfat-resize
COPYFILE_DISABLE=1 tar -xOzf "$archive" "$binary_entry" >"$binary"
chmod 0755 "$binary"

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
output_name=$package.signed
output_binary=$output_directory/$output_name
output_log=$output_directory/$output_name.notarization.json
[ ! -e "$output_binary" ] || fail "output executable already exists: $output_binary"
[ ! -e "$output_log" ] || fail "notarization log already exists: $output_log"

identities=$(security find-identity -v -p codesigning)
if ! printf '%s\n' "$identities" | grep -F "$identity" >/dev/null; then
	echo "$identities" >&2
	fail "code-signing identity is not available: $identity"
fi

codesign --force --identifier exfat-resize --options runtime --timestamp \
	--sign "$identity" "$binary"
codesign --verify --strict --verbose=2 "$binary"
signing_details=$(codesign -dvvv "$binary" 2>&1)
identifier=$(printf '%s\n' "$signing_details" | sed -n 's/^Identifier=//p')
authority=$(printf '%s\n' "$signing_details" | sed -n 's/^Authority=//p' | sed -n '1p')
team_identifier=$(printf '%s\n' "$signing_details" | sed -n 's/^TeamIdentifier=//p')
[ "$identifier" = exfat-resize ] || fail "signed CLI has an unexpected identifier: $identifier"
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
if ! printf '%s\n' "$signing_details" | grep -F 'flags=0x10000(runtime)' >/dev/null; then
	fail "signed CLI does not have only the hardened-runtime code-signing flag"
fi
entitlements=$(codesign -d --entitlements - "$binary" 2>/dev/null)
if [ -n "$entitlements" ]; then
	fail "signed CLI has unexpected entitlements"
fi
if [ "$("$binary" --version)" != "exfat-resize $build_version" ]; then
	fail "signed CLI version is incorrect"
fi

notary_archive=$temporary/$package-notarization.zip
COPYFILE_DISABLE=1 ditto -c -k --keepParent "$binary" "$notary_archive"
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

notarization_attempt=1
while ! notarization_check=$(codesign -vvvv -R=notarized \
	--check-notarization "$binary" 2>&1); do
	if [ "$notarization_attempt" -ge 6 ]; then
		printf '%s\n' "$notarization_check" >&2
		fail "notarization ticket is unavailable for the signed CLI"
	fi
	notarization_attempt=$((notarization_attempt + 1))
	sleep 5
done
printf '%s\n' "$notarization_check"

install -m 0755 "$binary" "$output_binary"
install -m 0644 "$notary_log" "$output_log"
output_sha256=$(shasum -a 256 "$output_binary" | awk '{ print $1 }')
printf 'built signed and notarized executable: %s\n' "$output_binary"
printf 'notarization log: %s\n' "$output_log"
printf 'Developer ID authority: %s\n' "$authority"
printf 'Team Identifier: %s\n' "$team_identifier"
printf 'output executable SHA-256: %s\n' "$output_sha256"
