# Contributing

Run the relevant tests before submitting a change. The commands below cover the
development build, test suites, sanitizer checks, and release verification. For
the shorter user-oriented build and installation path, see the
[README](README.md#build-from-source).

## Build and test

CMake 3.20 or newer is the canonical build system for the library, CLI, and
tests. Package tests require Git.

### macOS and Linux

The top-level Makefile is a convenience wrapper around CMake and the release
scripts. If the macOS CMake application is installed without command-line
links, add its tools to `PATH`:

    export PATH=/Applications/CMake.app/Contents/bin:$PATH

Build the project and run its standard test suite with:

    make
    make test

`make` produces `build/exfat-resize`. `make test` builds every target and runs
the separately labeled library, package, and CLI suites through CTest. The CLI
tests use newfs_exfat and fsck_exfat on macOS, or mkfs.exfat and fsck.exfat on
Linux.

For broader validation, run:

    make sanitize-test

The sanitizer target runs all suites with AddressSanitizer and
UndefinedBehaviorSanitizer in a separate build directory. It also builds and
runs C and C++ `add_subdirectory()` consumers to verify the sanitized static
library's runtime link requirements.

When changing build, installation, packaging, or release behavior, or when
preparing a release, also run:

    make release-test

The release target creates and verifies the source archive from committed
`HEAD`, installs it into a temporary prefix, and builds C and C++ consumers
using both `find_package()` and `add_subdirectory()`. It requires `ssh-keygen` in
addition to the standard build and test prerequisites.

### Windows

The Windows build includes the CLI and library by default. It uses only native
Windows APIs and does not require a POSIX compatibility layer:

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build --build-config Release --output-on-failure -L "library|cli|package-native"

## C identifier namespaces

The project owns the `exfat_resize_` and `EXFAT_RESIZE_` namespaces.

Functions and objects used by only one translation unit must be `static`.
Cross-file internal functions and objects must use the `exfat_resize_` prefix
and be declared only in private headers. They are not public API unless
documented by `include/exfat_resize.h`.
