# Maintainer guide

This document records repository and release mechanics that are not part of
the user or contributor interface.

## Version identities

`VERSION` is the single manually maintained version value. It contains the
numeric CMake package version used for compatibility checks.

`tools/version.sh` resolves a separate build identity for the CLI:

1. A distribution archive supplies the identity through `.tarball-version`.
2. A Git checkout uses `git describe`, including a `-dirty` suffix when
   appropriate.
3. A source tree with neither form of provenance uses the package version with
   an `-unknown` suffix.

CMake passes the resolved identity to the CLI target as the private
`EXFAT_RESIZE_BUILD_VERSION` compile definition. Do not add another manually
maintained version constant.

`make dist` packages the committed source at `HEAD`; uncommitted changes are
deliberately excluded. At a matching `vX.Y.Z` tag, the archive and CLI use
`X.Y.Z`. Otherwise, their identity includes the `git describe` distance and
commit abbreviation. The archive records that identity in `.tarball-version`
and retains the packaged commit ID in Git's tar metadata.
