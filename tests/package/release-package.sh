#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

project_root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
cmake_command=${CMAKE:-cmake}
ctest_command=${CTEST:-ctest}
make_command=${MAKE:-make}
temporary=${TMPDIR:-/tmp}/exfat-resize-release-test.$$

cleanup() {
	rm -rf "$temporary"
}

build_consumer() {
	name=$1
	shift
	"$cmake_command" -S "$project_root/tests/package/consumer" \
		-B "$temporary/$name" "$@"
	"$cmake_command" --build "$temporary/$name" --parallel
	"$ctest_command" --test-dir "$temporary/$name" --output-on-failure
}

expect_version_rejected() {
	requested_version=$1
	name=$2
	log=$temporary/$name.log

	if "$cmake_command" -S "$project_root/tests/package/consumer" \
		-B "$temporary/$name" \
		-DCMAKE_PREFIX_PATH="$temporary/prefix" \
		-DEXFAT_RESIZE_FIND_VERSION="$requested_version" >"$log" 2>&1; then
		echo "unsupported package version $requested_version was accepted" >&2
		exit 1
	fi
	if ! grep -F "version: $package_version" "$log" >/dev/null; then
		echo "package rejection did not identify installed version $package_version" >&2
		cat "$log" >&2
		exit 1
	fi
}

trap cleanup EXIT HUP INT TERM
mkdir -p "$temporary"

cd "$project_root"
git status --short --untracked-files=all >"$temporary/status.before"
"$make_command" dist

package_version=$(cat build/dist/package-version)
build_version=$(cat build/dist/build-version)
archive=dist/exfat-resize-$build_version.tar.gz
archive_name=${archive##*/}
if [ ! -f "$archive" ] || [ ! -f "$archive.sha256" ]; then
	echo "release archive or checksum is missing" >&2
	exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
	(cd dist && sha256sum -c "$archive_name.sha256")
else
	(cd dist && shasum -a 256 -c "$archive_name.sha256")
fi
cp "$archive" "$temporary/reference.tar.gz"
cp "$archive.sha256" "$temporary/reference.tar.gz.sha256"

git clone --quiet --no-local "$project_root" "$temporary/dirty-helper-repository"
printf '%s\n' '' 'This uncommitted line is intentional.' \
	>>"$temporary/dirty-helper-repository/README.md"
"$cmake_command" -S "$temporary/dirty-helper-repository" \
	-B "$temporary/dirty-cli" \
	-DEXFAT_RESIZE_BUILD_CLI=ON \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF
"$cmake_command" --build "$temporary/dirty-cli" --parallel --target exfat-resize
dirty_cli_version=$("$temporary/dirty-cli/exfat-resize" --version)
if [ "$dirty_cli_version" != "exfat-resize $build_version-dirty" ]; then
	echo "dirty checkout CLI version does not identify the dirty tree" >&2
	exit 1
fi
(
	cd "$temporary/dirty-helper-repository"
	"$make_command" dist
)
cmp "$temporary/reference.tar.gz" \
	"$temporary/dirty-helper-repository/$archive"
cmp "$temporary/reference.tar.gz.sha256" \
	"$temporary/dirty-helper-repository/$archive.sha256"

tar -xzf "$temporary/reference.tar.gz" -C "$temporary"
source_tree=$temporary/exfat-resize-$build_version
if [ ! -d "$source_tree" ] || [ -e "$source_tree/.git" ]; then
	echo "release archive did not produce a Git-free source tree" >&2
	exit 1
fi
if [ "$(cat "$source_tree/VERSION")" != "$package_version" ]; then
	echo "archive and package version disagree" >&2
	exit 1
fi
if [ "$(cat "$source_tree/.tarball-version")" != "$build_version" ]; then
	echo "archive name and archived build version disagree" >&2
	exit 1
fi
archive_commit=$(gzip -dc "$temporary/reference.tar.gz" | git get-tar-commit-id)
if [ "$archive_commit" != "$(git rev-parse --verify 'HEAD^{commit}')" ]; then
	echo "archive provenance does not identify the packaged commit" >&2
	exit 1
fi

"$cmake_command" -S "$source_tree" -B "$temporary/archive-cli" \
	-DCMAKE_BUILD_TYPE=Release \
	-DEXFAT_RESIZE_BUILD_CLI=ON \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF
"$cmake_command" --build "$temporary/archive-cli" --parallel --target exfat-resize
archive_cli_version=$("$temporary/archive-cli/exfat-resize" --version)
if [ "$archive_cli_version" != "exfat-resize $build_version" ]; then
	echo "archive name and built CLI version disagree" >&2
	exit 1
fi

"$cmake_command" -S "$source_tree" -B "$temporary/package-build" \
	-DEXFAT_RESIZE_BUILD_CLI=ON \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF \
	-DCMAKE_INSTALL_PREFIX="$temporary/prefix"
"$cmake_command" --build "$temporary/package-build" --parallel
"$cmake_command" --install "$temporary/package-build"

cmp "$source_tree/LICENSE" "$temporary/prefix/share/doc/exfat_resize/LICENSE"
cmp "$source_tree/README.md" "$temporary/prefix/share/doc/exfat_resize/README.md"
installed_cli_version=$("$temporary/prefix/bin/exfat-resize" --version)
if [ "$installed_cli_version" != "exfat-resize $build_version" ]; then
	echo "installed CLI version disagrees with build version" >&2
	exit 1
fi
installed_manpage=$temporary/prefix/share/man/man8/exfat-resize.8
if [ ! -f "$installed_manpage" ] ||
	! grep -F "exfat-resize $build_version" "$installed_manpage" >/dev/null; then
	echo "installed manual page is missing or has the wrong version" >&2
	exit 1
fi

compatible_version=${package_version%.*}
major=${package_version%%.*}
patch=${package_version##*.}
incompatible_version=$((major + 1)).0.0
future_version=${package_version%.*}.$((patch + 1))

build_consumer find-package-exact \
	-DCMAKE_PREFIX_PATH="$temporary/prefix" \
	-DEXFAT_RESIZE_FIND_VERSION="$package_version" \
	-DEXFAT_RESIZE_FIND_EXACT=ON \
	-DEXFAT_RESIZE_EXPECTED_VERSION="$package_version"
build_consumer find-package-compatible \
	-DCMAKE_PREFIX_PATH="$temporary/prefix" \
	-DEXFAT_RESIZE_FIND_VERSION="$compatible_version" \
	-DEXFAT_RESIZE_EXPECTED_VERSION="$package_version"
expect_version_rejected "$incompatible_version" find-package-incompatible
expect_version_rejected "$future_version" find-package-future

build_consumer add-subdirectory-consumer \
	-DEXFAT_RESIZE_SOURCE_DIR="$source_tree"

"$cmake_command" -S "$source_tree" -B "$temporary/relative-include-build" \
	-DEXFAT_RESIZE_BUILD_CLI=OFF \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF \
	-DCMAKE_INSTALL_PREFIX="$temporary/relative-prefix" \
	-DCMAKE_INSTALL_INCLUDEDIR=custom/include
"$cmake_command" --build "$temporary/relative-include-build" --parallel
"$cmake_command" --install "$temporary/relative-include-build"
build_consumer relative-include-consumer \
	-DCMAKE_PREFIX_PATH="$temporary/relative-prefix" \
	-DEXFAT_RESIZE_FIND_VERSION="$package_version" \
	-DEXFAT_RESIZE_EXPECTED_VERSION="$package_version"

"$cmake_command" -S "$source_tree" -B "$temporary/absolute-include-build" \
	-DEXFAT_RESIZE_BUILD_CLI=OFF \
	-DEXFAT_RESIZE_BUILD_TESTS=OFF \
	-DCMAKE_INSTALL_PREFIX="$temporary/absolute-prefix" \
	-DCMAKE_INSTALL_INCLUDEDIR="$temporary/absolute-include"
"$cmake_command" --build "$temporary/absolute-include-build" --parallel
"$cmake_command" --install "$temporary/absolute-include-build"
build_consumer absolute-include-consumer \
	-DCMAKE_PREFIX_PATH="$temporary/absolute-prefix" \
	-DEXFAT_RESIZE_FIND_VERSION="$package_version" \
	-DEXFAT_RESIZE_EXPECTED_VERSION="$package_version"

git status --short --untracked-files=all >"$temporary/status.after"
if ! cmp -s "$temporary/status.before" "$temporary/status.after"; then
	echo "release-package test modified the source tree" >&2
	diff -u "$temporary/status.before" "$temporary/status.after" >&2 || true
	exit 1
fi

echo "release-package: passed"
