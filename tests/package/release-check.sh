#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

project_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-release-check.XXXXXX")
repository=$temporary/repository

cleanup() {
	rm -rf "$temporary"
}

fail() {
	echo "release-check test: $*" >&2
	exit 1
}

expect_rejected() {
	name=$1
	expected=$2
	shift 2
	log=$temporary/$name.log

	if "$@" >"$log" 2>&1; then
		fail "$name was accepted"
	fi
	if ! grep -F "$expected" "$log" >/dev/null; then
		echo "release-check test: $name did not report: $expected" >&2
		cat "$log" >&2
		exit 1
	fi
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$repository/tools"
cp "$project_root/tools/release-check.sh" "$repository/tools/release-check.sh"
cp "$project_root/tools/version.sh" "$repository/tools/version.sh"

git -C "$repository" init --quiet
git -C "$repository" config user.name "Release Check Test"
git -C "$repository" config user.email "release-check@example.invalid"

printf '%s\n' 1.2.3 >"$repository/VERSION"
git -C "$repository" add VERSION tools
git -C "$repository" commit --quiet -m "Release 1.2.3"
commit_123=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" tag v1.2.3 "$commit_123"

printf '%s\n' "same version, different commit" >"$repository/NOTES"
git -C "$repository" add NOTES
git -C "$repository" commit --quiet -m "Development change"
other_commit=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')

printf '%s\n' 2.0.0 >"$repository/VERSION"
git -C "$repository" add VERSION
git -C "$repository" commit --quiet -m "Release 2.0.0"
commit_200=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" tag -a -m "Release 2.0.0" v2.0.0 "$commit_200"

printf '%s\n' 3.0.0 >"$repository/VERSION"
git -C "$repository" add VERSION
git -C "$repository" commit --quiet -m "Mismatched release"
commit_300=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" tag v3.0.1 "$commit_300"

blob=$(printf '%s\n' "not a commit" | git -C "$repository" hash-object -w --stdin)
git -C "$repository" tag v4.0.0 "$blob"

git -C "$repository" status --short --untracked-files=all >"$temporary/status.before"
git -C "$repository" show-ref >"$temporary/refs.before"

helper=$repository/tools/release-check.sh
lightweight_output=$(sh "$helper" v1.2.3 "$commit_123")
expected_output="release-check: v1.2.3 identifies $commit_123 as version 1.2.3"
if [ "$lightweight_output" != "$expected_output" ]; then
	fail "lightweight tag reported unexpected output: $lightweight_output"
fi

external_helper=$project_root/tools/release-check.sh
annotated_output=$(sh "$external_helper" --source-dir "$repository" \
	v2.0.0 "$commit_200")
expected_output="release-check: v2.0.0 identifies $commit_200 as version 2.0.0"
if [ "$annotated_output" != "$expected_output" ]; then
	fail "annotated tag reported unexpected output: $annotated_output"
fi

for malformed_tag in 1.2.3 v1.2 v1.2.3.4 v1..3 v1.2.x v1.2.3-rc1; do
	expect_rejected "malformed-$malformed_tag" \
		"invalid release tag: $malformed_tag" \
		sh "$helper" "$malformed_tag" "$commit_123"
done

expect_rejected missing-tag "tag does not exist: v9.9.9" \
	sh "$helper" v9.9.9 "$commit_300"
expect_rejected non-commit-tag "tag does not identify a commit: v4.0.0" \
	sh "$helper" v4.0.0 "$commit_300"
expect_rejected invalid-commit "commit does not identify a commit: missing" \
	sh "$helper" v1.2.3 missing
expect_rejected wrong-target "not $other_commit" \
	sh "$helper" v1.2.3 "$other_commit"
expect_rejected version-mismatch \
	"VERSION 3.0.0 does not match tag version 3.0.1" \
	sh "$helper" v3.0.1 "$commit_300"
expect_rejected missing-source \
	"source directory not found: $temporary/missing" \
	sh "$external_helper" --source-dir "$temporary/missing" \
	v1.2.3 "$commit_123"

git -C "$repository" status --short --untracked-files=all >"$temporary/status.after"
git -C "$repository" show-ref >"$temporary/refs.after"
if ! cmp -s "$temporary/status.before" "$temporary/status.after"; then
	fail "helper modified the fixture working tree"
fi
if ! cmp -s "$temporary/refs.before" "$temporary/refs.after"; then
	fail "helper modified the fixture refs"
fi

echo "release-check: passed"
