# Release notes

Version-tag pushes and the existing manual release workflow create GitHub releases
with generated notes after signed-tag verification, source validation, protected
release approval, and blocker checks. Reruns preserve an existing release and its
curated text and assets. Existing drafts fail visibly and require maintainer
reconciliation before rerunning publication. Dry runs do not create releases.

The shared `omit-from-release-notes` PR label excludes a PR from generated notes.
`actions/labeler` applies it using the filename rules in `.github/labeler.yml`.
Every changed filename, including the previous name of a rename, must match the
single union of support paths. The guard checks the complete file count, stable
head/base revisions and the actual omission label after classification. Failed,
unexpectedly skipped or unverifiable classification attempts to remove the label
and keeps the workflow failed. Cleanup is best effort if GitHub access fails.
Before checkout or dependency installation, the workflow confirms that the target
is a PR. An ordinary issue number or unconfirmed lookup fails without changing
labels. Confirmed PRs retain cleanup after later setup or classification failures.
`sync-labels: true` removes it when those filename rules stop matching. Author
identity and broad `cicd`, `chore`, or dependency labels never suppress runtime changes.

The allowlist covers `tests/`, integration fixtures under `test/`, documentation
under `docs/`, `.github/`, README and notices, signing documentation, the existing
release-tool test files, root `.gitignore`, and named editor/license-check
configuration. C++ sources, public headers, CMake inputs, Makefile, examples,
release tools, license terms, and unknown paths remain visible. The blanket
`.github/**` allowance intentionally includes release configuration and
publication workflows; PRs changing only those paths can be omitted. The guard
shares the canonical labeler rules and retains `actions/labeler` for label
synchronization.

To backfill already merged PRs before a release, run **Label release notes**
from Actions on the default branch, supplying each PR number. For example:

```sh
gh workflow run label-release-notes.yml -f pull_request=123
```

Publication and classification remain asynchronous. Before creating a release tag,
list every merged PR in the release range and inspect its latest **Label release
notes** run and current `omit-from-release-notes` label. Require a successful final
verification log for the current head/base/file count and actual label; check that
mixed runtime/support PRs remain visible. Backfill missing, failed or stale results
from the default branch and wait for successful verification before releasing.
The workflow rejects manual dispatch from another branch before loading helpers;
it never checks out the classified PR's code. Older merged PRs use current trusted
default-branch tooling rather than their historical base snapshot.

Automated release/classifier synchronization is not provided. If manual
verification is missed, published notes can retain omissions or unwanted support
entries. Backfilling affects future generated notes; existing release bodies and
assets are preserved on reruns. No follow-up automated synchronization work is
scheduled.

Run the release-publication checks locally with
`npm ci --ignore-scripts --prefix .github` followed by
`node --test .github/tests/*.test.cjs`.
