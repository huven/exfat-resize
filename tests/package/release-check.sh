#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
set -f

project_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-release-check.XXXXXX")
repository=$temporary/repository
signing_key=$temporary/release-signing-key
other_signing_key=$temporary/other-signing-key

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
if ! command -v ssh-keygen >/dev/null 2>&1; then
	fail "ssh-keygen is required"
fi
ssh-keygen -q -t ed25519 -N "" -C "release check" -f "$signing_key"
ssh-keygen -q -t ed25519 -N "" -C "other signer" -f "$other_signing_key"

mkdir -p "$repository/cmake" "$repository/tools"
cp "$project_root/cmake/ExfatResizeVersion.cmake" \
	"$repository/cmake/ExfatResizeVersion.cmake"
cp "$project_root/tools/release-check.sh" "$repository/tools/release-check.sh"
cp "$project_root/tools/version.cmake" "$repository/tools/version.cmake"
signing_public_key=$(cat "$signing_key.pub")
printf '%s namespaces="git" %s\n' \
	"release-check@example.invalid" "$signing_public_key" \
	>"$repository/tools/release-signers"

git -C "$repository" init --quiet
git -C "$repository" config user.name "Release Check Test"
git -C "$repository" config user.email "release-check@example.invalid"

printf '%s\n' 1.2.3 >"$repository/VERSION"
git -C "$repository" add VERSION cmake tools
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
git -C "$repository" \
	-c gpg.format=ssh -c user.signingkey="$signing_key" \
	tag -s -m "Release 2.0.0" v2.0.0 "$commit_200"

printf '%s\n' 2.1.0 >"$repository/VERSION"
git -C "$repository" add VERSION
git -C "$repository" commit --quiet -m "Release 2.1.0"
commit_210=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" tag -a -m "Release 2.1.0" v2.1.0 "$commit_210"

printf '%s\n' 2.2.0 >"$repository/VERSION"
git -C "$repository" add VERSION
git -C "$repository" commit --quiet -m "Release 2.2.0"
commit_220=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" \
	-c gpg.format=ssh -c user.signingkey="$other_signing_key" \
	tag -s -m "Release 2.2.0" v2.2.0 "$commit_220"

printf '%s\n' 2.3.0 >"$repository/VERSION"
git -C "$repository" add VERSION
git -C "$repository" commit --quiet -m "Release 2.3.0"
commit_230=$(git -C "$repository" rev-parse --verify 'HEAD^{commit}')
git -C "$repository" \
	-c gpg.format=ssh -c user.signingkey="$signing_key" \
	tag -s -m "Release 2.3.0" v2.3.0 "$commit_230"
git -C "$repository" cat-file tag v2.3.0 | \
	sed 's/^tag v2\.3\.0$/tag v2.3.1/' >"$temporary/invalid-tag"
invalid_tag=$(git -C "$repository" hash-object -t tag -w \
	"$temporary/invalid-tag")
git -C "$repository" update-ref refs/tags/v2.3.1 "$invalid_tag"

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

signed_output=$(sh "$helper" --require-signed-tag v2.0.0 "$commit_200")
if [ "$signed_output" != "$expected_output" ]; then
	fail "signed tag reported unexpected output: $signed_output"
fi

expect_rejected signed-lightweight "release tag is not annotated: v1.2.3" \
	sh "$helper" --require-signed-tag v1.2.3 "$commit_123"
expect_rejected unsigned-annotated \
	"release tag does not have a valid release signature: v2.1.0" \
	sh "$helper" --require-signed-tag v2.1.0 "$commit_210"
expect_rejected different-signer \
	"release tag does not have a valid release signature: v2.2.0" \
	sh "$helper" --require-signed-tag v2.2.0 "$commit_220"
expect_rejected invalid-signature \
	"release tag does not have a valid release signature: v2.3.1" \
	sh "$helper" --require-signed-tag v2.3.1 "$commit_230"
expect_rejected source-supplied-signer \
	"release tag does not have a valid release signature: v2.0.0" \
	sh "$external_helper" --source-dir "$repository" --require-signed-tag \
	v2.0.0 "$commit_200"

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
