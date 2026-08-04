#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu
LC_ALL=C
export LC_ALL

if [ "$#" -ne 4 ]; then
	echo "usage: $0 PACKAGE_DIRECTORY PACKAGE_VERSION BUILD_VERSION SOURCE_DIRECTORY" >&2
	exit 2
fi

package_directory=$1
package_version=$2
build_version=$3
source_directory=$4

case $package_directory in
	/*) ;;
	*) package_directory=$(pwd)/$package_directory ;;
esac
case $source_directory in
	/*) ;;
	*) source_directory=$(pwd)/$source_directory ;;
esac

if [ "$(id -u)" -ne 0 ]; then
	echo "Debian package installation tests must run as root" >&2
	exit 1
fi
if [ "$(dpkg --print-architecture)" != amd64 ]; then
	echo "Debian package installation tests require amd64" >&2
	exit 1
fi

runtime_package=$package_directory/exfat-resize_${package_version}-1_amd64.deb
development_package=$package_directory/libexfat-resize-dev_${package_version}-1_amd64.deb
manifest=$package_directory/DEB_SHA256SUMS
for required_file in "$runtime_package" "$development_package" "$manifest"; do
	if [ ! -f "$required_file" ]; then
		echo "required package artifact not found: $required_file" >&2
		exit 1
	fi
done
if [ ! -d "$source_directory/tests/package/consumer" ]; then
	echo "package consumer source not found: $source_directory" >&2
	exit 1
fi

(
	cd "$package_directory"
	sha256sum --check DEB_SHA256SUMS
)

if [ "$(dpkg-deb -f "$runtime_package" Package)" != exfat-resize ] ||
	[ "$(dpkg-deb -f "$development_package" Package)" != libexfat-resize-dev ]; then
	echo "package metadata contains an unexpected package name" >&2
	exit 1
fi
for package in "$runtime_package" "$development_package"; do
	if [ "$(dpkg-deb -f "$package" Version)" != "$package_version-1" ] ||
		[ "$(dpkg-deb -f "$package" Architecture)" != amd64 ]; then
		echo "package metadata contains an unexpected version or architecture" >&2
		exit 1
	fi
	if [ "$(dpkg-deb -f "$package" Source)" != exfat-resize ]; then
		echo "package metadata contains an unexpected source package name" >&2
		exit 1
	fi
	if dpkg-deb --contents "$package" |
		awk '$2 != "root/root" { bad = 1 } END { exit bad ? 0 : 1 }'; then
		echo "package contains a file not owned by root:root: $package" >&2
		exit 1
	fi
done

runtime_depends=$(dpkg-deb -f "$runtime_package" Depends)
if ! printf '%s\n' "$runtime_depends" |
	grep -E '(^|, )libc6 \(>= [^)]+\)($|, )' >/dev/null; then
	echo "runtime package has no generated versioned libc6 dependency: $runtime_depends" >&2
	exit 1
fi
if [ "$(dpkg-deb -f "$runtime_package" Recommends)" != exfatprogs ]; then
	echo "runtime package has unexpected recommendations" >&2
	exit 1
fi
if [ "$(dpkg-deb -f "$development_package" Depends)" != libc6-dev ]; then
	echo "development package has unexpected dependencies" >&2
	exit 1
fi
if [ "$(dpkg-deb -f "$runtime_package" Description | sed -n '1p')" != \
	"Grow existing exFAT filesystems" ] ||
	[ "$(dpkg-deb -f "$development_package" Description | sed -n '1p')" != \
	"Development files for exfat-resize" ]; then
	echo "a package has an unexpected description synopsis" >&2
	exit 1
fi

runtime_files=$(dpkg-deb --contents "$runtime_package")
development_files=$(dpkg-deb --contents "$development_package")
for required_path in \
	./usr/bin/exfat-resize \
	./usr/share/man/man8/exfat-resize.8.gz \
	./usr/share/doc/exfat-resize/copyright \
	./usr/share/doc/exfat-resize/changelog.Debian.gz \
	./usr/share/doc/exfat-resize/README.md \
	./usr/share/doc/exfat-resize/docs/TRANSACTION.md; do
	printf '%s\n' "$runtime_files" | grep -F " $required_path" >/dev/null || {
		echo "runtime package is missing $required_path" >&2
		exit 1
	}
done
if printf '%s\n' "$runtime_files" |
	grep -E '/include/|libexfat_resize\.a|/cmake/exfat_resize/' >/dev/null; then
	echo "runtime package contains development files" >&2
	exit 1
fi
for required_pattern in \
	'/usr/include/exfat_resize.h' \
	'/usr/lib/.*/libexfat_resize.a' \
	'/usr/lib/.*/cmake/exfat_resize/exfat_resizeConfig.cmake' \
	'/usr/lib/.*/cmake/exfat_resize/exfat_resizeConfigVersion.cmake' \
	'/usr/lib/.*/cmake/exfat_resize/exfat_resizeTargets.cmake' \
	'/usr/share/doc/libexfat-resize-dev/changelog.Debian.gz' \
	'/usr/share/doc/libexfat-resize-dev/copyright'; do
	printf '%s\n' "$development_files" | grep -E "$required_pattern" >/dev/null || {
		echo "development package is missing a path matching $required_pattern" >&2
		exit 1
	}
done
if printf '%s\n' "$development_files" | grep -F '/usr/bin/' >/dev/null; then
	echo "development package contains a runtime executable" >&2
	exit 1
fi

if command -v lintian >/dev/null 2>&1; then
	lintian --no-user-dirs --ignore-lintian-env --allow-root \
		--fail-on error "$runtime_package" "$development_package"
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/exfat-resize-deb-test.XXXXXX")
installed_files=$temporary/installed-files
cleanup() {
	apt-get purge --yes libexfat-resize-dev exfat-resize >/dev/null 2>&1 || true
	rm -rf "$temporary"
}
trap cleanup EXIT HUP INT TERM

for package in "$runtime_package" "$development_package"; do
	dpkg-deb --contents "$package" |
		awk '$1 ~ /^[-l]/ { print $6 }' >>"$installed_files"
done

DEBIAN_FRONTEND=noninteractive apt-get install --yes \
	"$runtime_package" "$development_package"

if [ "$(exfat-resize --version)" != "exfat-resize $build_version" ]; then
	echo "installed executable has an unexpected build version" >&2
	exit 1
fi
gzip -dc /usr/share/man/man8/exfat-resize.8.gz |
	grep -F "exfat-resize $build_version" >/dev/null || {
	echo "installed manual page has an unexpected version" >&2
	exit 1
}
gzip -dc /usr/share/doc/exfat-resize/changelog.Debian.gz |
	grep -F "exfat-resize ($package_version-1)" >/dev/null || {
	echo "installed Debian changelog has an unexpected version" >&2
	exit 1
}
if [ -u /usr/bin/exfat-resize ] || [ -g /usr/bin/exfat-resize ]; then
	echo "installed executable must not be setuid or setgid" >&2
	exit 1
fi
if command -v getcap >/dev/null 2>&1 &&
	[ -n "$(getcap /usr/bin/exfat-resize)" ]; then
	echo "installed executable must not have file capabilities" >&2
	exit 1
fi

readelf -h /usr/bin/exfat-resize | grep -E 'Type:.*DYN' >/dev/null || {
	echo "installed executable is not position-independent" >&2
	exit 1
}
readelf -lW /usr/bin/exfat-resize | grep -F GNU_RELRO >/dev/null || {
	echo "installed executable has no RELRO segment" >&2
	exit 1
}
readelf -dW /usr/bin/exfat-resize |
	grep -E 'BIND_NOW|FLAGS.*NOW' >/dev/null || {
	echo "installed executable does not enable immediate binding" >&2
	exit 1
}
if readelf -lW /usr/bin/exfat-resize |
	awk '$1 == "GNU_STACK" && $0 ~ /RWE/ { found = 1 } END { exit found ? 0 : 1 }'; then
	echo "installed executable requests an executable stack" >&2
	exit 1
fi

cmake -S "$source_directory/tests/package/consumer" \
	-B "$temporary/consumer" \
	-DEXFAT_RESIZE_FIND_VERSION="$package_version" \
	-DEXFAT_RESIZE_FIND_EXACT=ON \
	-DEXFAT_RESIZE_EXPECTED_VERSION="$package_version"
cmake --build "$temporary/consumer" --parallel
ctest --test-dir "$temporary/consumer" --output-on-failure

image=$temporary/package-smoke.exfat
truncate -s 64M "$image"
mkfs.exfat "$image" >/dev/null
truncate -s 128M "$image"
exfat-resize "$image"
fsck.exfat -n "$image"

DEBIAN_FRONTEND=noninteractive apt-get purge --yes \
	libexfat-resize-dev exfat-resize
while IFS= read -r installed_file; do
	case $installed_file in
		./*) installed_file=/${installed_file#./} ;;
	esac
	if [ -e "$installed_file" ] || [ -L "$installed_file" ]; then
		echo "package removal left an installed file behind: $installed_file" >&2
		exit 1
	fi
done <"$installed_files"

echo "Debian package installation test: passed"
