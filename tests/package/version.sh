#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

project_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
temporary=${TMPDIR:-/tmp}/exfat-resize-version-test.$$

cleanup() {
	rm -rf "$temporary"
}

expect_version() {
	mode=$1
	commit=$2
	expected=$3

	if [ "$mode" = checkout ]; then
		actual=$(sh "$project_root/tools/version.sh" "$temporary/repository")
	else
		actual=$(sh "$project_root/tools/version.sh" --commit "$temporary/repository" "$commit")
	fi
	if [ "$actual" != "$expected" ]; then
		echo "$mode version: expected '$expected', got '$actual'" >&2
		exit 1
	fi
}

expect_tag_rejected() {
	mode=$1
	commit=$2
	log=$temporary/$mode-rejected.log

	if [ "$mode" = checkout ]; then
		if sh "$project_root/tools/version.sh" "$temporary/repository" >"$log" 2>&1; then
			echo "$mode accepted a tag that does not match VERSION" >&2
			exit 1
		fi
	else
		if sh "$project_root/tools/version.sh" --commit "$temporary/repository" "$commit" \
		    >"$log" 2>&1; then
			echo "$mode accepted a tag that does not match VERSION" >&2
			exit 1
		fi
	fi
	if ! grep -F "tag v2.0.0 does not match package version 1.2.3" "$log" >/dev/null; then
		echo "$mode rejection did not explain the tag mismatch" >&2
		cat "$log" >&2
		exit 1
	fi
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary/repository"
git -C "$temporary/repository" init --quiet
git -C "$temporary/repository" config user.name "exfat-resize test"
git -C "$temporary/repository" config user.email "test@example.invalid"
printf '%s\n' 1.2.3 >"$temporary/repository/VERSION"
git -C "$temporary/repository" add VERSION
git -C "$temporary/repository" commit --quiet -m "Matching version"
matching_commit=$(git -C "$temporary/repository" rev-parse HEAD)
git -C "$temporary/repository" tag v1.2.3

expected=$(printf '1.2.3\n1.2.3')
expect_version checkout "$matching_commit" "$expected"
expect_version commit "$matching_commit" "$expected"

printf '%s\n' mismatch >"$temporary/repository/change"
git -C "$temporary/repository" add change
git -C "$temporary/repository" commit --quiet -m "Mismatching tag"
mismatching_commit=$(git -C "$temporary/repository" rev-parse HEAD)
git -C "$temporary/repository" tag v2.0.0

expect_tag_rejected checkout "$mismatching_commit"
expect_tag_rejected commit "$mismatching_commit"

echo "version: passed"
