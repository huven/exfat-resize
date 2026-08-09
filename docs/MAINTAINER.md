# Maintainer guide

This document records repository and release mechanics that are not part of
the user or contributor interface.

## Version identities

`VERSION` is the single manually maintained version value. It contains the
numeric CMake package version used for compatibility checks.

`cmake/ExfatResizeVersion.cmake` reads that value directly and resolves a
separate build identity for the CLI:

1. A distribution archive supplies the identity through `.tarball-version`.
2. A Git checkout at a matching `vX.Y.Z` tag uses `X.Y.Z`; otherwise it uses
   `X.Y.Z-g<commit>`, with a fixed 12-character prefix of the full commit
   object ID. Modified tracked files add a `-dirty` suffix.
3. A source tree with neither form of provenance uses the package version with
   an `-unknown` suffix.

CMake only resolves the build identity when the CLI is enabled, then passes it
to the CLI target as the private
`EXFAT_RESIZE_BUILD_VERSION` compile definition. Do not add another manually
maintained version constant. `tools/version.cmake` uses the same implementation
to determine the committed version packaged by `make dist`.

`make dist` packages committed `HEAD`, excluding uncommitted changes, and
writes the source archive to `dist/`. At an exact matching `vX.Y.Z` tag, the
archive and CLI use `X.Y.Z`. Untagged archives are development snapshots whose
identity is `X.Y.Z-g<commit>`, independent of which other objects and tags are
present in the checkout. The archive stores that identity in
`.tarball-version` and the packaged commit ID in Git's tar metadata.

## Release procedure

The routine release procedure has three steps:

1. Update `VERSION` in a pull request, wait for all required checks, and merge
   it to `main`.
2. Check out the resulting release commit and create and push its signed
   `vX.Y.Z` tag using the command below.
3. Wait for the `Release` workflow, review its generated notes and the source
   and Linux assets in the draft GitHub Release, then publish the draft
   manually.

The maintainer does not run `make dist`, create the GitHub Release, calculate
checksums, or upload assets during a routine release.

The workflow creates or updates a draft release; it never modifies an already
published release. A failed run can therefore be rerun safely. Review the
generated notes and assets before publishing the draft manually.

## Creating a signed release tag

New releases use an annotated tag signed with the dedicated SSH release key.
After checking out the release commit and replacing `X.Y.Z` with the version
from `VERSION`, create the tag with:

    git -c gpg.format=ssh \
        -c user.signingkey="$HOME/.ssh/id_github_sign" \
        tag -s vX.Y.Z -m "exfat-resize X.Y.Z"

This command uses the private key directly, prompting for its passphrase,
and does not change the user's Git configuration. `-m` supplies the signed
tag message; the command creates no commit. Push the resulting tag with:

    git push origin vX.Y.Z

The strict release preflight verifies tags against `tools/release-signers`. The
authorized key currently has fingerprint
`SHA256:wdqKZjZosJpyhXgEqSbyse2xhwZdqfk99mncJHzrycY`. The private key must not
be committed, uploaded as a CI secret, or otherwise copied into CI.

## Release preflight

Release automation uses the read-only preflight check to validate the release
tag and expected commit before building artifacts. Maintainers may run the
same check locally when testing or diagnosing a release:

    tools/release-check.sh [--source-dir SOURCE_DIR] \
        [--require-signed-tag] vX.Y.Z COMMIT

The check requires an existing exact `vX.Y.Z` tag, verifies that it resolves to
the expected commit, compares the tag with the commit's `VERSION`, and requires
the version helper to report a final build identity rather than a development
identity. By default it accepts existing lightweight and annotated tags so
historical releases remain testable. `--require-signed-tag` additionally
requires an annotated SSH signature from a key in `tools/release-signers` and
is intended for the publishing path. `--source-dir` allows release tooling
from the current checkout to validate a separately checked-out tag; the signer
allowlist still comes from the release-tooling checkout.
