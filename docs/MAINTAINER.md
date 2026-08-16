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

The routine release procedure has six steps:

1. Update `VERSION` in a pull request, wait for all required checks, and merge
   it to `main`.
2. Check out the resulting release commit and create and push its signed
   `vX.Y.Z` tag using the command below.
3. Wait for the `Release` workflow to create the draft GitHub Release and reach
   the `macos-release` environment approval.
4. Download the unsigned macOS workflow artifact, then sign and notarize it as
   described below.
5. Upload the signed macOS executable to the draft and approve the waiting
   `macos-release` job.
6. Wait for GitHub to assemble and verify the signed archive, review the
   generated notes and all assets, then publish the draft manually.

The maintainer does not run `make dist`, create the GitHub Release, or calculate
release checksums. A temporary signed macOS executable is the only file uploaded
manually; the workflow removes it after constructing the final archive.

The workflow creates or updates a draft release; it never modifies an already
published release. If assembly or verification fails, use **Re-run failed jobs**
to retain and reuse the uploaded executable. **Re-run all jobs** rebuilds the
unsigned archive and reruns draft creation, which deletes every existing draft
asset. Download and verify the new workflow artifact, sign it again, and upload
its new `.signed` executable when the workflow returns to the approval. Neither
case requires replacing the release tag. Review the generated notes and assets
before publishing the draft manually.

The repository must have a protected GitHub environment named `macos-release`
with the maintainer as a required reviewer. Do not enable prevention of
self-review for a release initiated by that maintainer. The environment stores
no signing credentials; it only holds the verification job until the signed
executable has been uploaded to the draft.

## Signing the macOS executable

The `Test` and `Release` workflows retain the tested, ad-hoc-signed macOS
archive as a workflow artifact. Download that artifact separately; do not give
the signing helper GitHub credentials. When using Safari, Option-click the
artifact download link so Safari downloads it without automatically opening and
decompressing it. GitHub displays the SHA-256 of the downloaded artifact ZIP on
the workflow run page.

Before extracting the artifact, compare that displayed digest with the ZIP, then
extract it and run the local preflight:

    shasum -a 256 macos-unsigned-vX.Y.Z-RUN_ID.zip
    unzip macos-unsigned-vX.Y.Z-RUN_ID.zip
    tools/sign-macos-binary.sh --check exfat-resize-X.Y.Z-macos-arm64.tar.gz

The ZIP digest must match the value on the artifact download page. Because the
artifact is immutable, that check also authenticates the enclosed `.tar.gz`.

Signing requires a Developer ID Application certificate in the login Keychain
and notarization credentials stored under a `notarytool` Keychain profile. List
the available signing identities and create the profile before the first
signing run:

    security find-identity -v -p codesigning
    xcrun notarytool store-credentials exfat-resize-notary

Sign and notarize the executable from the archive into a separate output
directory:

    tools/sign-macos-binary.sh \
        --identity "Developer ID Application: NAME (TEAMID)" \
        --notary-profile exfat-resize-notary \
        exfat-resize-X.Y.Z-macos-arm64.tar.gz signed

Before accessing the signing identity, the helper selectively extracts exactly
one regular CLI entry and validates the archive version, architecture, deployment
target, dependencies, and ad-hoc signature without executing the downloaded CLI.
It then signs the CLI with the hardened runtime and a secure timestamp, submits a
temporary ZIP to Apple, retrieves the notarization log, and writes
`exfat-resize-X.Y.Z-macos-arm64.signed` alongside that log. Because Apple does not
support stapling a ticket to a standalone executable, the helper verifies the
ticket with the online notarization check supported by `codesign`. Upload only the
`.signed` executable to the draft GitHub Release, then approve the pending
`macos-release` environment job.

The approved job downloads that exact draft asset and its own tested unsigned
archive on a fresh macOS runner. It verifies that removing and canonicalizing the
signatures leaves the same executable GitHub built, then replaces only the CLI in
the trusted unsigned archive. It checks the GitHub asset digests, package version
and contents, ARM64 architecture, deployment target, dependencies, Developer ID
team, code-signing identifier, secure timestamp, hardened runtime, absence of
entitlements, and online notarization ticket. It also exercises installation and
uninstallation and performs an actual exFAT resize. After uploading the verified
final archive, it removes the temporary `.signed` asset. The job never publishes
the draft; publication remains a separate manual action after all checks pass.

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
