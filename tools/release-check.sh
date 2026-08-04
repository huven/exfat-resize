#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	echo "usage: $0 [--source-dir SOURCE_DIR] [--require-signed-tag] TAG COMMIT" >&2
	exit 2
}

fail() {
	echo "release-check: $*" >&2
	exit 1
}

validate_tag() {
	value=$1
	case $value in
	v*)
		version=${value#v}
		;;
	*)
		fail "invalid release tag: $value"
		;;
	esac

	case $version in
	""|*[!0-9.]*)
		fail "invalid release tag: $value"
		;;
	esac

	old_ifs=$IFS
	IFS=.
	set -- $version
	IFS=$old_ifs
	if [ "$#" -ne 3 ] || [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
		fail "invalid release tag: $value"
	fi

	printf '%s\n' "$version"
}

if ! tool_root=$(CDPATH= cd -- "$(dirname "$0")/.." 2>/dev/null && pwd); then
	fail "could not determine the release tooling directory"
fi
cmake_command=${CMAKE:-cmake}
source_dir=$tool_root
require_signed_tag=false

while [ "$#" -gt 0 ]; do
	case $1 in
	--source-dir)
		if [ "$#" -lt 2 ]; then
			usage
		fi
		if ! source_dir=$(CDPATH= cd -- "$2" 2>/dev/null && pwd); then
			fail "source directory not found: $2"
		fi
		shift 2
		;;
	--require-signed-tag)
		require_signed_tag=true
		shift
		;;
	--)
		shift
		break
		;;
	-*)
		usage
		;;
	*)
		break
		;;
	esac
done

if [ "$#" -ne 2 ]; then
	usage
fi
tag=$1
commit=$2

tag_version=$(validate_tag "$tag")
tag_ref=refs/tags/$tag

if ! git -C "$source_dir" show-ref --verify --quiet "$tag_ref"; then
	fail "tag does not exist: $tag"
fi
if [ "$require_signed_tag" = true ]; then
	if [ "$(git -C "$source_dir" cat-file -t "$tag_ref" 2>/dev/null)" != tag ]; then
		fail "release tag is not annotated: $tag"
	fi
	signers=$tool_root/tools/release-signers
	if [ ! -f "$signers" ]; then
		fail "release signer allowlist not found: $signers"
	fi
	if ! git -C "$source_dir" \
		-c gpg.format=ssh \
		-c gpg.ssh.allowedSignersFile="$signers" \
		verify-tag "$tag_ref" >/dev/null 2>&1; then
		fail "release tag does not have a valid release signature: $tag"
	fi
fi
if ! tag_commit=$(git -C "$source_dir" rev-parse --verify \
	"${tag_ref}^{commit}" 2>/dev/null); then
	fail "tag does not identify a commit: $tag"
fi
if ! expected_commit=$(git -C "$source_dir" rev-parse --verify \
	"${commit}^{commit}" 2>/dev/null); then
	fail "commit does not identify a commit: $commit"
fi
if [ "$tag_commit" != "$expected_commit" ]; then
	fail "tag $tag identifies $tag_commit, not $expected_commit"
fi

if ! package_version=$(git -C "$source_dir" show \
	"${expected_commit}:VERSION" 2>/dev/null); then
	fail "VERSION not found in commit $expected_commit"
fi
if [ "$package_version" != "$tag_version" ]; then
	fail "VERSION $package_version does not match tag version $tag_version"
fi

if ! version_output=$(mktemp -d \
	"${TMPDIR:-/tmp}/exfat-resize-release-check.XXXXXX"); then
	fail "could not create temporary version output directory"
fi
cleanup() {
	rm -rf "$version_output"
}
trap cleanup EXIT HUP INT TERM

if ! "$cmake_command" \
	"-DEXFAT_RESIZE_SOURCE_DIR=$source_dir" \
	"-DEXFAT_RESIZE_COMMIT=$expected_commit" \
	"-DEXFAT_RESIZE_VERSION_OUTPUT_DIR=$version_output" \
	-P "$tool_root/tools/version.cmake"; then
	fail "could not determine the release version"
fi
if ! resolved_package_version=$(cat "$version_output/package-version") ||
	! build_version=$(cat "$version_output/build-version"); then
	fail "version helper did not produce the expected output"
fi
if [ "$resolved_package_version" != "$tag_version" ]; then
	fail "version helper reported package version $resolved_package_version, expected $tag_version"
fi
if [ "$build_version" != "$tag_version" ]; then
	fail "version helper reported development build $build_version for tag $tag"
fi

printf 'release-check: %s identifies %s as version %s\n' \
	"$tag" "$expected_commit" "$tag_version"
