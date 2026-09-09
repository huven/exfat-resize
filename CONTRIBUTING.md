# Contributing

Run the relevant tests before submitting a change. The commands below cover the
development build, test suites, sanitizer checks, and release verification. For
the shorter user-oriented build and installation path, see the
[README](README.md#build-from-source).

## Build and test

CMake 3.20 or newer, a C11 compiler, and Git are required. From the repository
root, use the same commands on Linux, macOS, and Windows:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DEXFAT_RESIZE_BUILD_CLI=ON -DEXFAT_RESIZE_BUILD_TESTS=ON
    cmake --build build --config Release --parallel
    ctest --test-dir build --build-config Release --output-on-failure --no-tests=error

The configure command explicitly enables the CLI and tests, including when
reusing a build directory from the README's installation instructions. CTest
runs every registered test for the platform and reports an error if none are
found. The library, package, and CLI suites have separate labels; use `-L library`
or `-L cli` for a focused run after the full suite has passed.

### Platform prerequisites

On macOS, the image tests use the system's `hdiutil`, `newfs_exfat`, and
`fsck_exfat` tools. If CMake.app is installed without command-line links, add
its tools to `PATH`:

    export PATH=/Applications/CMake.app/Contents/bin:$PATH

On Linux, the image tests require `mkfs.exfat` and `fsck.exfat` from exfatprogs,
`mount.exfat-fuse` from exfat-fuse, `losetup`, and `sudo` for device operations.
The POSIX package checks also require `sh` and `ssh-keygen`.

On Windows, run the common commands with the compiler available in your
terminal. The registered tests use native Windows tools and APIs and require
no POSIX compatibility layer.

## Sanitizer checks (macOS and Linux)

With Clang or GCC, run all suites with AddressSanitizer and
UndefinedBehaviorSanitizer in a separate build directory:

    cmake -S . -B build/sanitize -DCMAKE_BUILD_TYPE=Debug \
        -DEXFAT_RESIZE_BUILD_CLI=ON -DEXFAT_RESIZE_BUILD_TESTS=ON \
        -DEXFAT_RESIZE_ENABLE_SANITIZERS=ON
    cmake --build build/sanitize --config Debug --parallel
    ctest --test-dir build/sanitize --build-config Debug --output-on-failure --no-tests=error

Also build and run the C and C++ `add_subdirectory()` consumers to verify the
sanitized static library's runtime link requirements. This needs a C++ compiler
in addition to the C compiler. Run these commands from the repository root:

    cmake -S tests/package/consumer -B build/sanitize-consumer -DCMAKE_BUILD_TYPE=Debug \
        -DEXFAT_RESIZE_SOURCE_DIR="$PWD" \
        -DEXFAT_RESIZE_BUILD_CLI=OFF -DEXFAT_RESIZE_BUILD_TESTS=OFF \
        -DEXFAT_RESIZE_ENABLE_SANITIZERS=ON
    cmake --build build/sanitize-consumer --config Debug --parallel
    ctest --test-dir build/sanitize-consumer --build-config Debug --output-on-failure --no-tests=error

## Release verification (macOS and Linux)

When changing build, installation, packaging, or release behavior, or when
preparing a release, also run:

    sh tests/package/release-check.sh
    sh tests/package/release-package.sh

These checks require `ssh-keygen` and a C++ compiler in addition to the standard
build and test prerequisites. They validate release tags, create and verify the
source archive from committed `HEAD`, install it into a temporary prefix, and
build C and C++ consumers using both `find_package()` and `add_subdirectory()`.
Commit the changes to be verified before running the package check; uncommitted
changes are excluded from the archive.

## C identifier namespaces

The project owns the `exfat_resize_` and `EXFAT_RESIZE_` namespaces.

Functions and objects used by only one translation unit must be `static`.
Cross-file internal functions and objects must use the `exfat_resize_` prefix
and be declared only in private headers. They are not public API unless
documented by `include/exfat_resize.h`.
