# Releases and signing

## Operator lifecycle

Stable releases are dispatched from `main` using **Release** (`release.yml`) or
**Release dry run** (`release-dry-run.yml`). Tag pushes do not publish releases.
Historical releases and prereleases are outside this workflow. Start from a clean,
reviewed `main` revision; if remote `main` advances, start validation again.

`VERSION` initially contains `0.1.0-SNAPSHOT`. CMake uses its numeric `X.Y.Z`
portion for project and installed-package compatibility. The existing public
version macro and user agent use the complete value, including `-SNAPSHOT`.
The optional `release-version` input must be stable `X.Y.Z`, without leading
zeroes, at least the numeric `VERSION`, and unused by any local/remote tag or
GitHub release. The credentialed real release job also rejects existing drafts.
Read-only validation and dry-run tokens cannot list drafts, so their success does
not establish that no draft exists. Without an override, use numeric `VERSION`.

The real workflow accepts `skip-tests` (default `false`). This skips only project
unit and integration execution. Helper tests, the AWS and core builds, warning
checks, license checks, archive verification, installation and downstream core/AWS
CMake consumer builds always run. The dry run has no skip input and runs every
check. Both workflows run validation without production signing credentials and
with read-only permissions, then request protected `release` environment approval.

Validation preserves launch-revision helpers before selecting source. It constructs
an unsigned release tree whose only change is `VERSION=X.Y.Z`, runs checks in an
isolated worktree, and records the baseline commit, selected version and tree ID.
Open issues **and pull requests** labelled `release-blocker` stop real releases
before validation, after validation, after approval, before the atomic push and
immediately before publishing the draft. Pagination and API errors fail closed.
Dry runs skip blocker checks.

After approval, a fresh runner reconstructs that same tree. A shared release
concurrency group serializes both workflows with `cancel-in-progress: false`.
Only trusted launch helpers run in the credentialed job; no source build commands,
selected-source hooks, executables or caches run there. Preparation creates:

1. A signed commit containing `VERSION=X.Y.Z`, directly on the recorded baseline.
2. A signed annotated `vX.Y.Z` tag on the release commit.
3. A signed development commit containing `X.Y.(Z+1)-SNAPSHOT`, directly on the
   release commit.

Public-key-only verification independently checks both signatures, the exact parent
chain, the tag object/name/target and both complete version-only trees. Source
archives are generated from the release commit with a common
`alternator-client-cpp-X.Y.Z/` root. Exactly six public assets are published:

- `alternator-client-cpp-X.Y.Z.tar.gz` and its `.asc` signature.
- `alternator-client-cpp-X.Y.Z.zip` and its `.asc` signature.
- `SHA256SUMS` and its `.asc` signature.

The archives contain tracked source, build files, documentation and licenses.
They contain no compiled binaries. Before any remote changes, Actions must retain
`release-recovery-VERSION-RUN-ATTEMPT`: `history.bundle` (the exact signed Git
objects), those six assets, and `manifest.json` plus its detached signature. The
signed manifest identifies the baseline, release/development commits, tree, tag
object and SHA-256 hashes of the bundle and all assets. Artifacts expire after
seven days; download recovery evidence promptly and keep it until publication
is reconciled.

Real publication rechecks blockers, unused version and remote `main`, then pushes
the development commit and tag together with `git push --atomic`, without force.
It creates a new GitHub **draft** with generated notes, uploads all six assets,
checks names, states, sizes and server SHA-256 digests, rechecks blockers and refs,
and publishes the draft. It requires immutable publication. This follows
[GitHub's draft-first release sequence](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository).
Dry runs perform the same signed preparation, packaging and public
verification, retaining the same artifacts without pushing refs or creating a
GitHub release. No registries, Sonatype credentials or notifications are involved.

## Workflow stages and public verification

New releases and dry runs are supported through GitHub Actions only. Dispatch
**Release** or **Release dry run** from `main` and use the workflow inputs to select
a version and, for real releases, whether to skip project tests.

`lifecycle.py` exposes the internal stages used by the workflow actions:
`plan`, `materialize`, `recheck`, `prepare`, `verify` and `publish`. Each stage
accepts only its own arguments and requires explicit plan or recovery paths.
The plan stage takes an explicit baseline and optional version override; the
preparation action reconstructs and checks the validated tree before signing.
The workflows gate preparation and publication on protected environment approval.

Public verification remains available without a publisher account or secrets:

```sh
python3 scripts/release/signing.py verify-tag vX.Y.Z
python3 scripts/release/signing.py verify-commit COMMIT
python3 scripts/release/lifecycle.py verify --output /path/to/recovery
```

## Signing certificate and isolation

The public certificate in `publisher.asc` is safe to commit and is registered on
`scylladb-publisher`. Obtain it from a reviewed repository revision and confirm
its fingerprint through a trusted channel. Consumer verification must require the
pinned signing fingerprint for every signature; importing this certificate into a
keyring alone does not constrain which key GnuPG accepts. The README uses an
isolated temporary keyring, disables automatic key retrieval/import and checks
each successful `VALIDSIG` record against the signing fingerprint below.

- Certification fingerprint: `04E63A9B9DC6BCE2BA182CBFB1F1949F8B9A0FC6`.
- Signing fingerprint: `15F46DE2ED437CB62E4C6F4BBA5DD9E6771F70D5`.
- Identity: `ScyllaDB Publisher (scylladb/alternator-client-cpp) <publish.code@scylladb.com>`.
- Expiration: `2028-10-19T15:45:24+00:00`.

Signing secrets belong only in the protected `release` environment:
`GPG_PRIVATE_KEY` (ASCII-armored protected signing-subkey export) and
`GPG_PASSPHRASE` (single-line passphrase). Keep the primary private key offline.
Actions operators may be approved maintainers other than the publisher account;
signatures always use the pinned repository subkey and publisher identity.
Every production-signing or publication job must declare `environment: release`.

Each signing stage uses a scoped `signing_session` or `signing.py exec` session. The session imports
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
their base64, hexadecimal, URL, JSON, XML and shell-escaped forms are redacted.
This includes double URL encoding, case-insensitive percent escapes, and encoded
username/password pairs. Authorization headers and credential assignments are
masked even when their values were not supplied to the helper. Overlapping
matches are combined against the original bytes before inserting redaction markers. Raw
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
follow the [source-release verification instructions](../../README.md#source-releases).
That procedure uses an isolated keyring, disables automatic key retrieval and
requires the pinned signing fingerprint.

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

## Deployment protection and rollout

The `release` environment must allow branch `main` only, require approval from
`scylladb/drivers`, allow self-review, disable administrator bypass and hold both
signing secrets. Keep the existing immutable `v*` tag ruleset active: tag update
and deletion are forbidden with no bypass actors. Enable repository release
immutability before the first release. YAML cannot establish these settings.
Environment ref rules apply to the workflow launch ref.
Checking the repository immutability setting through the API requires
administration read access, which the workflow's `contents: write` token lacks.
Administrators must keep it enabled; the publisher checks the resulting release's
immutable status and stops for manual inspection if that confirmation fails.

Settings were updated and read back on 2026-11-05: release immutability is
`enabled=true`; deployment branch policy ID `63996649` permits only branch `main`
(the former `v*` deployment policy was removed). Drivers team ID `8797733` remains
the reviewer, `prevent_self_review=false` and `can_admins_bypass=false`. The
separate immutable-tag ruleset ID `25740501` was preserved without modification.

After merge and settings updates, dispatch **Release dry run** from `main`, obtain
real environment approval, download the seven-day artifact and inspect the signed
manifest, exact history, archives, checksums, public signatures and sanitized logs.
Confirm remote refs/releases are unchanged. Record the run URL before the first
real release. Local tests and API readback do not establish that an approved dry
run has occurred. Do not retire old secret copies until that rehearsal succeeds.

Release actors and reviewed launch revisions remain trusted. An authorized actor
who modifies the workflow or runs an old workflow can bypass its internal policy;
keep repository access and external tag restrictions in place. Release immutability
protects published tags and assets; it does not replace review or approval.

## Recovery from partial publication

Stop concurrent release runs and preserve the Actions run URL and recovery
artifact. **Do not rerun the entire workflow after any remote change.** Never
recreate, re-sign, move, delete or force-push a published tag; never overwrite an
asset. A timeout is an unknown outcome until GitHub and Git are inspected.

First verify the manifest signature with a trusted copy of `publisher.asc`, then
verify every hash in `manifest.json`. Import the exact objects into a fresh local
repository, without regenerating commits or signatures:

```sh
git init recovery-work
cd recovery-work
git fetch /absolute/path/to/history.bundle \
  refs/release/development:refs/release/development \
  refs/tags/vX.Y.Z:refs/tags/vX.Y.Z
python3 /path/to/reviewed/scripts/release/lifecycle.py verify \
  --output /absolute/path/to/recovery-artifact
```

Compare `manifest.json` with `git ls-remote` and inspect **all** GitHub releases,
including drafts (`gh api --paginate repos/scylladb/alternator-client-cpp/releases`).
API errors never establish absence. Recheck open issue/PR blockers and repository
protections before any manual publication. Retain the original six files unchanged.

- If preparation or artifact upload failed before the push, no remote refs should
  have changed. Confirm that before abandoning local prepared state and starting
  a fresh validation. A failed dry run should always leave remote state unchanged.
- If atomic push was rejected, confirm neither remote ref changed. If `main`
  advanced, discard the unpushed preparation and validate the new baseline.
- If push succeeded but no draft exists, confirm the remote tag object equals
  `tag_object` and remote main is the retained development commit (or investigate
  later work), then manually create the draft for that **existing** tag with
  generated notes and `--verify-tag`.
- If draft creation timed out, find the existing draft before taking another action.
  If uploads failed, download and hash-check assets already present. Upload only
  missing assets from the retained recovery set, without `--clobber`. Any mismatch
  requires investigation. Verify all six names, sizes, SHA-256 digests and detached
  signatures before publishing the existing draft.
- Immediately before publishing, recheck issue/PR blockers and the exact remote
  tag. Publish only the verified draft. If publication timed out, inspect its
  `draft` and `immutable` fields and its assets. If it is published with the exact
  retained assets, treat publication as complete. Never use a new workflow run to
  finish it. A conflicting published release needs maintainer investigation.

If the recovery artifact is unavailable, do not recreate a signed object under
an existing release tag. Escalate to maintainers with the surviving Git/API evidence.
