#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

program=$1
temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-guidance.XXXXXX")
no_write_guidance="exfat-resize: no filesystem write was attempted; correct the error and retry when appropriate"

cleanup() {
	rm -rf "$temporary"
}

expect_no_write_failure() {
	name=$1
	shift
	output=$temporary/$name.out

	if "$program" "$@" >"$output" 2>&1; then
		echo "$name unexpectedly succeeded" >&2
		exit 1
	fi
	if ! grep -F "$no_write_guidance" "$output" >/dev/null; then
		echo "$name did not report the no-write recovery boundary" >&2
		cat "$output" >&2
		exit 1
	fi
	if grep -F "restore the verified backup" "$output" >/dev/null ||
	    grep -F "filesystem checker" "$output" >/dev/null; then
		echo "$name reported destructive recovery guidance" >&2
		cat "$output" >&2
		exit 1
	fi
}

expect_guidance_free_success() {
	name=$1
	shift
	output=$temporary/$name.out

	if ! "$program" "$@" >"$output" 2>&1; then
		echo "$name unexpectedly failed" >&2
		cat "$output" >&2
		exit 1
	fi
	if grep -F "$no_write_guidance" "$output" >/dev/null; then
		echo "$name printed unnecessary recovery guidance" >&2
		cat "$output" >&2
		exit 1
	fi
}

trap cleanup EXIT HUP INT TERM

expect_no_write_failure missing-target
expect_no_write_failure unknown-option --unknown
expect_no_write_failure invalid-size "$temporary/missing" 0
expect_no_write_failure target-open "$temporary/missing"

expect_guidance_free_success short-help -h
expect_guidance_free_success long-help --help
expect_guidance_free_success short-version -V
expect_guidance_free_success long-version --version

echo "recovery-guidance: passed"
