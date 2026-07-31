#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	echo "usage: $0 [--source-dir SOURCE_DIR] TAG COMMIT" >&2
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

case $# in
2)
	if ! source_dir=$(CDPATH= cd -- "$(dirname "$0")/.." 2>/dev/null && pwd); then
		fail "could not determine the source directory"
	fi
	tag=$1
	commit=$2
	;;
4)
	if [ "$1" != "--source-dir" ]; then
		usage
	fi
	if ! source_dir=$(CDPATH= cd -- "$2" 2>/dev/null && pwd); then
		fail "source directory not found: $2"
	fi
	tag=$3
	commit=$4
	;;
*)
	usage
esac

tag_version=$(validate_tag "$tag")
tag_ref=refs/tags/$tag

if ! git -C "$source_dir" show-ref --verify --quiet "$tag_ref"; then
	fail "tag does not exist: $tag"
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

if ! version_info=$(sh "$source_dir/tools/version.sh" --commit \
	"$source_dir" "$expected_commit"); then
	fail "could not determine the release version"
fi
resolved_package_version=$(printf '%s\n' "$version_info" | sed -n '1p')
build_version=$(printf '%s\n' "$version_info" | sed -n '2p')
if [ "$resolved_package_version" != "$tag_version" ]; then
	fail "version helper reported package version $resolved_package_version, expected $tag_version"
fi
if [ "$build_version" != "$tag_version" ]; then
	fail "version helper reported development build $build_version for tag $tag"
fi

printf 'release-check: %s identifies %s as version %s\n' \
	"$tag" "$expected_commit" "$tag_version"
