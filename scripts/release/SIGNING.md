# Repository release signing

The public certificate in `publisher.asc` is safe to commit and is registered on
`scylladb-publisher`. Keep it available to consumers; it does not belong in secrets.

* Certification fingerprint: `04E63A9B9DC6BCE2BA182CBFB1F1949F8B9A0FC6`
* Signing fingerprint: `15F46DE2ED437CB62E4C6F4BBA5DD9E6771F70D5`
* Identity: `ScyllaDB Publisher (scylladb/alternator-client-cpp) <publish.code@scylladb.com>`
* Expiration: `2028-10-19T15:45:24+00:00`

Signing secrets are provisioned in the protected `release` environment:
`GPG_PRIVATE_KEY` (ASCII-armored protected signing-subkey export) and
`GPG_PASSPHRASE` (single-line passphrase). The current release-notes workflow uses
only the public certificate and does not need either secret.
Never upload the full primary private-key backup to CI. Signatures use the same
signing subkey, selected by its complete fingerprint with `!`. The primary key
certifies the signing subkey and remains offline.

Local `make release-tag` and internal `_release-tag` calls require the authenticated
`gh` account `scylladb-publisher` before importing credentials or creating tags;
switch accounts with `gh auth switch` if needed. Local Make dry runs use the same
policy. Actions operators are authorized separately by the protected `release`
environment. Approved maintainers can dispatch or rerun workflows without being
the publisher account; signatures still use the pinned publisher subkey.
`GITHUB_ACTIONS=true` only selects this policy branch; it does not prove approval.
Public verification needs no account check. The local CLI identity does not
establish which credentials a later Git push will use.

Each signing stage runs in a scoped `signing.py exec` session. The session imports
only the protected signing subkey, checks that primary private material is absent,
and verifies a fresh-agent signing preflight against the pinned public certificate.
Anonymous pipes supply the key export and passphrase; the helper does not stage
either input in a temporary file or command-line argument. Imported keys stay in
the temporary keyring. Git configuration applies only to the command and its
children, including cloned checkouts. Existing runtime Git settings are preserved.
Recursive Make cannot restore the consumed private-key inputs from command-line
overrides. Temporary keyrings are removed on success, failure, or catchable
cancellation, without changing checkout configuration or writing state to
`GITHUB_ENV`. Owned command groups are stopped before keyring cleanup. Commands
that create separate process sessions must manage those sessions themselves.
Runner cleanup is still needed after an uncatchable kill or machine failure.

For maintainer-driven tag creation:

```sh
export RELEASE_GPG_PRIVATE_KEY_FILE=/private/path/signing-private.asc
export RELEASE_GPG_PASSPHRASE_FILE=/private/path/passphrase.txt
export RELEASE_VERSION=X.Y.Z
make release-tag
RELEASE_TAG=vX.Y.Z make release-verify-tag
```

`release-tag` validates the account and version before opening the signing session,
then creates a signed annotated tag at the current commit. Versions may use
`X.Y.Z`, `X.Y.Z-alpha.1`, `X.Y.Z-beta.1`, or `X.Y.Z-rc.1`. A retry reuses an
existing tag only when its signature matches the pinned key and it still points
to the current commit. Do not change or replace existing tags. These targets
create local outputs; pushing the tag triggers the release-notes workflow.

Complex pre-release checks can run before tagging or against signed outputs
after the session closes. Checks needing signatures can run in their own session:

```sh
python3 scripts/release/signing.py exec make YOUR_CHECK_TARGET
```

The low-level signing helper does not impose release-account policy. Wrapped
commands have no overall time limit; each GPG operation has its own timeout and
reads the current passphrase, so long checks do not depend on an agent cache.
A failed command propagates its exit status and still cleans up the session.
Agent-shutdown failures produce a fixed warning without replacing that status.
Helper failures report a fixed operation label and exit status or error category,
without including command arguments, captured output, or exception payloads.
Wrapped commands inherit stdout/stderr directly; this helper does not sanitize
arbitrary build output. Commands receiving a passphrase must not print it or
enable shell tracing.
Run `make test-release-signing` to exercise signing with disposable keys.

Direct artifact signing uses the same isolated session and credential inputs:

```sh
python3 scripts/release/signing.py sign-files package.tar.gz SHA256SUMS
```

Existing signatures are verified and reused without overwriting them. If every
signature already exists, verification requires only public material. When using
the Python API inside an existing session, pass its environment explicitly as
`sign_files(files, signing_env)`.

Consumers should obtain this public certificate from this repository at a reviewed
commit, check the certification fingerprint above through a trusted channel, and
verify release signatures. The Ubuntu keyserver can also serve the certificate:

```sh
gpg --keyserver hkps://keyserver.ubuntu.com --recv-keys 04E63A9B9DC6BCE2BA182CBFB1F1949F8B9A0FC6
gpg --fingerprint 04E63A9B9DC6BCE2BA182CBFB1F1949F8B9A0FC6
gpg --status-fd 1 --verify ARTIFACT.asc ARTIFACT
```

Require one `GOODSIG` with the signing subkey's final 16 fingerprint characters
and one `VALIDSIG` with the full signing and primary fingerprints above.
Reject `BADSIG`, `ERRSIG`, `EXPSIG`,
`EXPKEYSIG`, `REVKEYSIG`, `KEYREVOKED`, `KEYEXPIRED`, and `SIGEXPIRED` status records,
as well as `FAILURE` and `ERROR`, even when `VALIDSIG` is also present.
`git verify-tag --raw TAG` and
`git verify-commit --raw COMMIT` provide the equivalent check for Git objects.
Keyserver presence alone establishes neither trust nor the expected repository
identity.

Rotate by preparing a new independent certificate, validating its protected subkey,
registering its public certificate on the publisher account, publishing the public
certificate, reviewing fingerprint/public-certificate updates together, and updating
the secret pair in a coordinated release pause. Keep old public certificates available
for historical releases. Never revoke the previous key merely to rotate it. If a key
is compromised, stop releases and use the offline revocation certificate; fix the
exposure before resuming. Do not rerun publication after a partial release without
following the repository's recovery instructions.

## Current release path

The release-notes workflow verifies existing annotated tags with this certificate;
it does not import a private key or create version commits. Maintainers create
release tags through `make release-tag` above. No release or development commits
are prepared, so there is no new commit range to verify. The existing tagged
commit need not be signed by the release key. Artifact uploads and Maven
prepare/perform stages are outside this release path.

The workflow preserves the launch revision's helper and public certificate,
verifies the requested tag directly from fetched refs, then publishes release
notes without checking out historical build code. Dispatch input selects an
existing tag name, never a preceding commit or a replacement tag. A failed
signature check blocks publication.

## Deployment protection and rollout

The repository's `release` environment was provisioned and read back through the
GitHub API on 2026-10-22 with these settings:

* One required approval from the `scylladb/drivers` team.
* Self-review allowed; administrator bypass disabled.
* Allowed launch refs: branch `main` and tags matching `v*`.
* Environment secrets: `GPG_PRIVATE_KEY` and `GPG_PASSPHRASE`.

The protected signing-subkey export and exact passphrase were validated against
this repository's pinned certificate in an isolated signing session before upload.
No primary private key was uploaded. Environment secret names and update times
were verified after upload; GitHub does not return stored secret values.

Environment ref rules apply to the launch ref, not the tag named in dispatch input.
Configure the corresponding release-tag ruleset as described below. YAML's
`environment: release` binding does not create reviewer or ref protections.
Recheck the environment settings before releasing; local tests and API readback
do not replace a real approved workflow rehearsal.

If CI signing is added, provision the environment secret pair first, gate every
signing or publishing job, then migrate the workflow. Remove repository-level
copies only after the migration and an approved rehearsal succeed. The current
workflow does not import private material and cannot exercise that migration.

Tests cover real Make/GPG/Git signing with disposable credentials, artifact
tampering, maintainer dispatch/reruns, failure cleanup, and cancellation. They
preserve persistent Git settings and verify temporary keyring removal. They do
not dispatch a real approved GitHub release or upload artifacts. Release-signing
tests run in pull-request CI.

Automatic publication assumes trusted release actors and external restrictions
on release-tag creation, updates, and deletion. Configure a repository tag ruleset
to limit those operations to trusted release actors, who must tag commits with a
reviewed release workflow. The local `gh` account check does not authenticate a
later Git push. Keep release tags immutable so verification and publication refer
to the same object.

On a tag push, GitHub loads the workflow, helper, and certificate from the tagged
commit. An old or modified workflow can omit all of these checks. Copying policy
inside that workflow, or adding an environment check only to its current version,
does not prevent that bypass. Dispatching an old or modified workflow has the same
limitation. The environment does not configure the separate release-tag ruleset;
the workflow is not an independent security boundary against authorized tag creators.
