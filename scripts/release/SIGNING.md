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
switch accounts with `gh auth switch` if needed. Local Make dry runs check the
account and version without opening signing credentials. Actions release commands
and publication require `scylladb-publisher` as the effective initiator:
`GITHUB_TRIGGERING_ACTOR`, falling back to `GITHUB_ACTOR` when unset or empty.
This also checks the operator who requests a rerun. The protected `release`
environment requires approval independently of that account check.
`GITHUB_ACTIONS=true` only selects this policy branch; it does not prove approval.
Public verification needs no account check. The local CLI identity does not
establish which credentials a later Git push will use.

Each signing stage runs in a scoped `signing.py exec` session. The session imports
only the protected signing subkey, checks that primary private material is absent,
and verifies a fresh-agent signing preflight against the pinned public certificate.
Anonymous pipes supply the key export and passphrase; the helper does not stage
either input in a temporary file or command-line argument. Imported keys stay in
the temporary keyring. These pipes protect delivery to GnuPG; wrapped commands
and their descendants inherit `RELEASE_GPG_PASSPHRASE` or
`RELEASE_GPG_PASSPHRASE_FILE` and can read the passphrase or its file. Treat all
commands run through `signing.py exec` as trusted recipients of those inputs.
Git configuration applies only to the command and its children, including cloned
checkouts. Existing runtime Git settings are preserved.
Recursive Make cannot restore the consumed private-key inputs from command-line
overrides. Temporary keyrings are removed on success, failure, or catchable
cancellation, without changing checkout configuration or writing state to
`GITHUB_ENV`. Guarded commands require Linux with `/proc`. The guard temporarily
acts as a child subreaper, tracks descendants across new process sessions and
double forks, and stops/reaps them before keyring cleanup. It preserves existing
children and restores the caller's original subreaper setting. If ownership cannot
be established, it refuses to start the command.
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
Wrapped commands run through `guard.py`: stdout and stderr are captured separately
in bounded memory (8 MiB combined), then sanitized before either stream is printed
or saved. Known credential values, input-file paths, keyring/wrapper paths, and
their base64, hexadecimal, URL, JSON and shell-escaped forms are redacted. Raw
command output is never spooled to disk. Set `RELEASE_LOG_DIR` to save sanitized
`stdout.log` and `stderr.log`, including after command failure or cancellation.
Overflow or incomplete capture withholds the streams and fails a successful
command. Log-write failures also fail success, while preserving a command's
nonzero status or cancellation. Credential-bearing argv, shell tracing and common
debug/trace modes are rejected before private-key import. Guard errors disclose
only fixed diagnostics.

The guard is not a sandbox or a defense against deliberately transformed or
exfiltrated credentials. Commands still receive the passphrase inputs and must be
trusted. Do not print credentials, dump the environment, or enable tracing.
Run `make test-release-signing` to exercise signing with disposable keys.

Direct artifact signing uses the same isolated session and credential inputs:

```sh
python3 scripts/release/signing.py sign-files package.tar.gz SHA256SUMS
```

Signatures use `FILE.asc` beside the supplied artifact path, including when that
path is a symlink. Existing signatures are verified and reused without overwriting
them. If every signature already exists, verification requires only public material.
When using the Python API inside an existing session, pass its environment explicitly as
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

The normal release-notes path verifies existing annotated tags with this certificate;
it does not import a private key or create version commits. The protected rehearsal
mode imports the environment subkey only for its isolated signing preflight. Maintainers create
release tags through `make release-tag` above. No release or development commits
are prepared, so there is no new commit range to verify. The existing tagged
commit need not be signed by the release key. Artifact uploads and Maven
prepare/perform stages are outside this release path.

The validation job runs the launch revision's release helper/guard tests and uses
its public certificate to verify the requested tag. It records the exact tag
object and target commit. A separate, read-only invocation of the existing C++
test workflow checks out that recorded commit and runs build, license/lint, unit
and integration checks. It receives no production signing secrets and has no
deployment environment. Selected source code executes only on validation runners.

Only after both validations succeed does the `release` environment request
approval. A fresh publication runner checks out the launch revision's trusted
tools, checks the same tag object and commit, and repeats public verification.
There is no workspace, build-output or cache handoff into this runner. It checks
the remote tag object again immediately before creating release notes with
`--verify-tag`. Dispatch input selects an existing tag, never a preceding commit
or replacement tag. Failed/cancelled/skipped validation prevents publication and
approval. External tag immutability remains required to close concurrent remote
updates; no sequence of client-side checks makes tag mutation atomic with the
GitHub Release API.

For an upload-free rehearsal after merge, dispatch `release.yml` from an allowed
launch ref (normally `main`), select an existing repository-signed `target-tag`,
and set `dry-run=true`. The same helper and exact-source C++ validation runs first.
After approval, the protected job uses the environment secret pair for a fresh,
isolated signing preflight and independent public verification, stops the agent,
and removes its temporary keyring. It uploads only sanitized command logs and
never calls `gh release create`, uploads packages, creates/replaces tags or pushes
commits. This is distinct from `make -n release-tag`, which only prints a local
recipe. A successful rehearsal still requires real GitHub approval; adding this
mode does not establish that one has run.

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

The protected rehearsal now exercises environment-scoped signing on a fresh
runner. Keep every future signing or publishing job behind the same environment.
Remove repository-level secret copies only after the environment-aware workflow
has landed and an approved rehearsal succeeds. Until then, migration remains
incomplete; the existence of this rehearsal mode is not evidence that it ran.

Tests cover real Make/GPG/Git signing with disposable credentials, artifact
tampering, publisher authorization and reruns, failure cleanup, cancellation,
detached descendants, bounded/redacted logs, and the exact workflow shell blocks
against a bare remote and fresh checkout. They reject moved/unsigned tag objects
and prevent publication after the remote target changes.
They preserve persistent Git settings and verify temporary keyring removal. They do
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

## Recovery from a partial release

Stop concurrent release runs before recovery. Preserve the original tag name,
signed object ID, target commit and Actions run URL. Never delete, move, re-sign or
force-push a published tag to make a retry pass. Never remove old public keys or
credentials as part of an unverified recovery.

* If local `release-tag` failed, verify the existing local tag with
  `RELEASE_TAG=vX.Y.Z make release-verify-tag` and confirm its commit equals the
  intended source. Rerunning `make release-tag` reuses it only when both its
  signature and commit match. A mismatch requires investigation, not replacement.
* If the tag was pushed but validation failed, fix the validation problem first.
  Dispatch the workflow from the reviewed allowed launch ref with that same tag;
  its unchanged object and source undergo fresh validation before approval. An
  intentional source change requires a new version/tag.
* If approval was rejected/cancelled or the protected job failed before release
  creation, confirm the remote tag is unchanged and check whether a GitHub Release
  already exists before rerunning. A rerun is still subject to publisher identity,
  validation dependencies and environment approval. Use a new dispatch when the
  previous validation or launch tools are no longer suitable.
* If creation timed out or its result is uncertain, first inspect
  `gh release view vX.Y.Z --repo scylladb/alternator-client-cpp` and the remote tag
  object. If the intended published release already exists, treat creation as
  complete; do not rerun `gh release create`. Review its tag, notes and draft state
  manually. A conflicting or draft release needs an explicit maintainer decision;
  this workflow does not overwrite or automatically publish it.
* If no release exists and the signed remote tag still matches the retained object
  and commit, a new dispatch can safely repeat validation/approval and attempt
  creation. Do not infer absence from an authentication, network or API error.
* A failed rehearsal publishes nothing. Correct the reported issue and repeat the
  rehearsal with the same immutable tag. Keep its approval and sanitized-log
  evidence before retiring repository-level signing-secret copies.

The current path has no package upload or prepared version commits to reconcile.
If either is added, define its destination-specific recovery before enabling it.
