#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

usage() {
	echo "usage: $0 SOURCE_DIR" >&2
	echo "       $0 --commit SOURCE_DIR COMMIT" >&2
	exit 2
}

fail() {
	echo "version: $*" >&2
	exit 1
}

validate_package_version() {
	value=$1
	case $value in
	""|*[!0-9.]*)
		fail "invalid package version: $value"
		;;
	esac

	old_ifs=$IFS
	IFS=.
	set -- $value
	IFS=$old_ifs
	if [ "$#" -ne 3 ] || [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ]; then
		fail "invalid package version: $value"
	fi
}

validate_build_version() {
	value=$1
	case $value in
	""|[!0-9A-Za-z]*|*[!0-9A-Za-z.+-]*)
		fail "invalid build version: $value"
		;;
	esac
}

describe_checkout() {
	source_dir=$1
	package_version=$2

	if [ -f "$source_dir/.tarball-version" ]; then
		build_version=$(cat "$source_dir/.tarball-version")
	elif [ -e "$source_dir/.git" ] && command -v git >/dev/null 2>&1; then
		exact_tag=
		if candidate=$(git -C "$source_dir" describe --exact-match --tags \
		    --match "v[0-9]*" 2>/dev/null); then
			exact_tag=$candidate
		fi
		if [ -n "$exact_tag" ] && [ "$exact_tag" != "v$package_version" ]; then
			fail "tag $exact_tag does not match package version $package_version"
		fi
		if ! description=$(git -C "$source_dir" describe --tags \
		    --match "v[0-9]*" --always --dirty 2>/dev/null); then
			fail "could not describe Git checkout"
		fi
		case $description in
		v*)
			build_version=${description#v}
			;;
		*)
			build_version=$package_version-g$description
			;;
		esac
	else
		build_version=$package_version-unknown
	fi

	printf '%s\n' "$build_version"
}

describe_commit() {
	source_dir=$1
	package_version=$2
	commit=$3

	exact_tag=
	if candidate=$(git -C "$source_dir" describe --exact-match --tags \
	    --match "v[0-9]*" "$commit" 2>/dev/null); then
		exact_tag=$candidate
	fi
	if [ -n "$exact_tag" ] && [ "$exact_tag" != "v$package_version" ]; then
		fail "tag $exact_tag does not match package version $package_version"
	fi
	if ! description=$(git -C "$source_dir" describe --tags \
	    --match "v[0-9]*" --always "$commit" 2>/dev/null); then
		fail "could not describe commit $commit"
	fi
	case $description in
	v*)
		build_version=${description#v}
		;;
	*)
		build_version=$package_version-g$description
		;;
	esac

	printf '%s\n' "$build_version"
}

case $# in
1)
	source_dir=$1
	if [ ! -f "$source_dir/VERSION" ]; then
		fail "VERSION not found in $source_dir"
	fi
	package_version=$(cat "$source_dir/VERSION")
	validate_package_version "$package_version"
	build_version=$(describe_checkout "$source_dir" "$package_version")
	;;
3)
	if [ "$1" != "--commit" ]; then
		usage
	fi
	source_dir=$2
	commit=$3
	if ! package_version=$(git -C "$source_dir" show "${commit}:VERSION" 2>/dev/null); then
		fail "VERSION not found in commit $commit"
	fi
	validate_package_version "$package_version"
	build_version=$(describe_commit "$source_dir" "$package_version" "$commit")
	;;
*)
	usage
	;;
esac

validate_build_version "$build_version"
printf '%s\n%s\n' "$package_version" "$build_version"
