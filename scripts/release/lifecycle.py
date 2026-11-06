#!/usr/bin/env python3
# Copyright ScyllaDB, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Stable release planning, signed history, source assets and draft-first publication.

Only launch-revision copies of this module and signing.py run with credentials.
No build files, hooks, filters or executables from the selected tree are executed.
"""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

import check_blockers
import signing

STABLE = r"(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)"
OID = r"[0-9a-f]{40}"


class ReleaseError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ReleaseError(message)


def command(args, *, data=None, env=None, operation=None):
    # Captured failures must never print credential-bearing subprocess diagnostics.
    operation = operation or signing.command_operation(args)
    try:
        result = subprocess.run(args, input=data, capture_output=True, env=env, timeout=180)
    except subprocess.TimeoutExpired:
        raise ReleaseError(operation + " timed out; inspect remote state before recovery.") from None
    except OSError:
        raise ReleaseError(operation + " could not start; inspect remote state before recovery.") from None
    require(result.returncode == 0,
            f"{operation} failed (exit status {result.returncode}); inspect remote state before recovery.")
    return result.stdout


def git(*args, data=None, env=None):
    return command(["git", "-c", "core.hooksPath=/dev/null", *map(str, args)], data=data, env=env)


def oid(ref):
    return git("rev-parse", "--verify", ref).decode().strip()


def version(value):
    require(isinstance(value, str) and re.fullmatch(STABLE, value), "Expected stable version X.Y.Z")
    return tuple(map(int, value.split(".")))


def next_version(value):
    major, minor, patch = version(value)
    return f"{major}.{minor}.{patch + 1}-SNAPSHOT"


def version_tree(baseline, value):
    require(re.fullmatch(OID, baseline), "Invalid baseline object")
    # A private index changes only VERSION and leaves the checkout/index untouched.
    with tempfile.TemporaryDirectory(prefix="release-index-") as temp:
        env = {**os.environ, "GIT_INDEX_FILE": str(Path(temp) / "index")}
        git("read-tree", baseline, env=env)
        blob = git("hash-object", "-w", "--stdin", data=(value + "\n").encode()).decode().strip()
        git("update-index", "--add", "--cacheinfo", "100644", blob, "VERSION", env=env)
        return git("write-tree", env=env).decode().strip()


def remote_refs():
    rows = git("ls-remote", "--refs", "origin", "refs/heads/main", "refs/tags/*").decode().splitlines()
    return dict((name, obj) for obj, name in (row.split() for row in rows))


def api(repository, endpoint, *args, operation="GitHub API request"):
    require(re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository), "Expected OWNER/REPOSITORY")
    return json.loads(command(["gh", "api", f"repos/{repository}/{endpoint}", *args], operation=operation))


def releases(repository):
    pages = api(repository, "releases?per_page=100", "--paginate", "--slurp", operation="GitHub release listing")
    require(isinstance(pages, list) and bool(pages) and all(isinstance(p, list) for p in pages),
            "Cannot verify existing releases")
    rows = [row for page in pages for row in page]
    require(all(isinstance(row, dict) and isinstance(row.get("tag_name"), str) for row in rows),
            "Cannot verify existing releases")
    return rows


def available(plan, repository):
    refs = remote_refs()
    require(refs.get("refs/heads/main") == plan["baseline"], "Remote main advanced; start a new validation")
    require("refs/tags/" + plan["tag"] not in refs, "Release tag already exists")
    require(not any(row["tag_name"] == plan["tag"] for row in releases(repository)),
            "Release or draft already exists; use manual recovery")


def plan_release(override="", baseline=None):
    require(os.environ.get("GITHUB_REF") == "refs/heads/main",
            "Releases must be dispatched from main")
    baseline = baseline or oid("HEAD")
    require(re.fullmatch(OID, baseline) and oid(baseline + "^{commit}") == baseline, "Invalid baseline")
    source = git("show", baseline + ":VERSION").decode().removesuffix("\n")
    numeric = source.removesuffix("-SNAPSHOT")
    minimum = version(numeric)
    selected = override or numeric
    require(version(selected) >= minimum, "Release version is below VERSION")
    require(not git("for-each-ref", "--format=%(refname)", "refs/tags/v" + selected).strip(),
            "Release tag already exists locally")
    return {"baseline": baseline, "version": selected, "tag": "v" + selected,
            "tree": version_tree(baseline, selected)}


def validate_plan(plan):
    require(isinstance(plan, dict) and set(plan) == {"baseline", "version", "tag", "tree"}, "Invalid release plan")
    version(plan["version"])
    require(all(isinstance(plan[k], str) and re.fullmatch(OID, plan[k]) for k in ("baseline", "tree")),
            "Invalid planned objects")
    require(plan["tag"] == "v" + plan["version"], "Invalid planned tag")
    require(version_tree(plan["baseline"], plan["version"]) == plan["tree"], "Validated release tree changed")
    minimum = git("show", plan["baseline"] + ":VERSION").decode().strip().removesuffix("-SNAPSHOT")
    require(version(plan["version"]) >= version(minimum), "Release version is below VERSION")
    return plan


def write_json(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write("\n")


def read_plan(path):
    return validate_plan(json.loads(Path(path).read_text()))


def prepare(plan, output):
    validate_plan(plan)
    require(oid("HEAD") == plan["baseline"], "Checkout differs from validated baseline")
    require(not git("status", "--porcelain", "--untracked-files=no").strip(), "Tracked checkout changes are not allowed")
    require(not git("for-each-ref", "--format=%(refname)", "refs/tags/" + plan["tag"]).strip(),
            "Release tag already exists locally; use manual recovery")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    release = git("commit-tree", "-S", plan["tree"], "-p", plan["baseline"], "-m",
                  "Release " + plan["version"]).decode().strip()
    git("tag", "-s", "-m", "Release " + plan["version"], plan["tag"], release)
    development = git("commit-tree", "-S", version_tree(release, next_version(plan["version"])),
                      "-p", release, "-m", "Begin " + next_version(plan["version"])).decode().strip()
    git("update-ref", "refs/release/development", development)
    identity = {**plan, "release": release, "development": development,
                "tag_object": oid("refs/tags/" + plan["tag"])}
    verify_history(identity)
    assets = output / "assets"
    assets.mkdir()
    archives = package(release, plan["version"], assets)
    sums = assets / "SHA256SUMS"
    sums.write_text("".join(f"{digest(path)}  {path.name}\n" for path in archives))
    signing.sign_files([*archives, sums], os.environ)
    git("bundle", "create", str((output / "history.bundle").resolve()),
        "refs/release/development", "refs/tags/" + plan["tag"])
    manifest = {"identity": identity, "sha256": {
        str(p.relative_to(output)): digest(p) for p in [output / "history.bundle", *sorted(assets.iterdir())]}}
    write_json(output / "manifest.json", manifest)
    signing.sign_files([output / "manifest.json"], os.environ)
    return identity


def prepare_signed(plan, output, repository):
    available(plan, repository)
    with signing.signing_session() as env, signing.session_signals():
        # commit-tree/Git GPG wrapper inherit only this scoped environment.
        previous = dict(os.environ)
        try:
            os.environ.clear()
            os.environ.update(env)
            prepare(plan, output)
        finally:
            os.environ.clear()
            os.environ.update(previous)


def verify_history(identity):
    plan = {k: identity[k] for k in ("baseline", "version", "tag", "tree")}
    validate_plan(plan)
    for key, parent, expected_tree in (
            ("release", plan["baseline"], plan["tree"]),
            ("development", identity["release"], version_tree(identity["release"], next_version(plan["version"])))):
        commit = identity[key]
        require(isinstance(commit, str) and re.fullmatch(OID, commit), "Invalid prepared commit")
        require(git("show", "-s", "--format=%P", commit).decode().strip() == parent,
                "Prepared history has unexpected parents")
        require(oid(commit + "^{tree}") == expected_tree, "Prepared commit changes files beyond VERSION")
        signing.verify_commit(commit)
    require(oid("refs/tags/" + plan["tag"]) == identity["tag_object"], "Prepared tag object changed")
    signing.verify_tag(plan["tag"], identity["release"])


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def asset_names(value):
    stem = "alternator-client-cpp-" + value
    unsigned = [stem + ".tar.gz", stem + ".zip", "SHA256SUMS"]
    return set(unsigned + [name + ".asc" for name in unsigned])


def package(ref, value, directory):
    version(value)
    prefix = "alternator-client-cpp-" + value + "/"
    paths = [Path(directory) / (prefix[:-1] + suffix) for suffix in (".tar.gz", ".zip")]
    raw = git("-c", "core.attributesFile=/dev/null", "archive", "--format=tar", "--prefix=" + prefix, ref)
    with paths[0].open("xb") as stream:
        stream.write(gzip.compress(raw, mtime=0))
    with paths[1].open("xb") as stream:
        stream.write(git("-c", "core.attributesFile=/dev/null", "archive", "--format=zip", "--prefix=" + prefix, ref))
    for path in paths:
        verify_archive(path, ref, value)
    return paths


def verify_archive(path, ref, value):
    prefix = "alternator-client-cpp-" + value + "/"
    expected = {}
    for row in git("ls-tree", "-rz", ref).split(b"\0"):
        if not row:
            continue
        meta, name = row.split(b"\t", 1)
        mode, kind, blob = meta.decode().split()
        require(kind == "blob" and mode in ("100644", "100755", "120000"), "Unsupported source tree entry")
        expected[prefix + name.decode()] = git("cat-file", "blob", blob)
    actual = {}
    def add(name, data):
        require(name.startswith(prefix) and ".." not in PurePosixPath(name).parts and name not in actual,
                "Unsafe or duplicate archive member")
        actual[name] = data
    if str(path).endswith(".tar.gz"):
        with tarfile.open(path, "r:gz") as archive:
            for member in archive:
                if member.isdir():
                    require(member.name == prefix[:-1] or member.name.startswith(prefix), "Unsafe archive directory")
                    continue
                require(member.isfile() or member.issym(), "Unsupported archive member")
                add(member.name, member.linkname.encode() if member.issym() else archive.extractfile(member).read())
    else:
        with zipfile.ZipFile(path) as archive:
            for member in archive.infolist():
                if member.is_dir():
                    require(member.filename.startswith(prefix), "Unsafe archive directory")
                    continue
                add(member.filename, archive.read(member))
    require(actual == expected, "Archive does not contain the exact tracked release source")
    require(actual.get(prefix + "VERSION") == (value + "\n").encode(), "Archive VERSION mismatch")


def verify_file(path):
    with signing.public_environment() as env:
        signing.verify_status(signing.gpg("--status-fd", "1", "--verify", str(path) + ".asc", path, env=env))


def verify_recovery(output):
    output = Path(output)
    verify_file(output / "manifest.json")
    manifest = json.loads((output / "manifest.json").read_text())
    identity = manifest["identity"]
    verify_history(identity)
    names = asset_names(identity["version"])
    expected = {"history.bundle", *("assets/" + name for name in names)}
    require(set(manifest["sha256"]) == expected, "Recovery manifest has unexpected files")
    require({p.name for p in (output / "assets").iterdir()} == names, "Expected exactly six public assets")
    for name, sha in manifest["sha256"].items():
        require(digest(output / name) == sha, "Recovery file hash mismatch")
    git("bundle", "verify", str((output / "history.bundle").resolve()))
    heads = dict((name, obj) for obj, name in (line.split() for line in
                 git("bundle", "list-heads", str((output / "history.bundle").resolve())).decode().splitlines()))
    require(heads == {"refs/release/development": identity["development"],
                      "refs/tags/" + identity["tag"]: identity["tag_object"]}, "Bundle refs differ from manifest")
    assets = output / "assets"
    for name in names:
        if not name.endswith(".asc"):
            verify_file(assets / name)
    archives = [assets / ("alternator-client-cpp-" + identity["version"] + suffix) for suffix in (".tar.gz", ".zip")]
    require((assets / "SHA256SUMS").read_text() == "".join(f"{digest(p)}  {p.name}\n" for p in archives),
            "Checksum file mismatch")
    for archive in archives:
        verify_archive(archive, identity["release"], identity["version"])
    return identity


def publish(output, repository):
    identity = verify_recovery(output)
    available(identity, repository)
    require(check_blockers.check(repository) == 0, "Open release blockers")
    # Both refs succeed together or neither does; never force or follow unrelated tags.
    git("push", "--atomic", "--no-follow-tags", "origin", identity["development"] + ":refs/heads/main",
        "refs/tags/" + identity["tag"] + ":refs/tags/" + identity["tag"])
    require_remote(identity)
    require(not any(row["tag_name"] == identity["tag"] for row in releases(repository)),
            "Release or draft appeared after push; use manual recovery")
    command(["gh", "release", "create", identity["tag"], "--repo", repository, "--draft",
             "--generate-notes", "--title", identity["tag"], "--verify-tag"], operation="GitHub draft creation")
    # Tag lookup is documented for published releases. Locate the new draft in
    # the authenticated release listing and pin its ID for every subsequent API call.
    drafts = [row for row in releases(repository) if row["tag_name"] == identity["tag"]]
    require(len(drafts) == 1, "Cannot uniquely identify the new release draft")
    release = drafts[0]
    require(release.get("draft") is True and release.get("prerelease") is False
            and type(release.get("id")) is int, "Unexpected draft release")
    release_id = release["id"]
    assets = Path(output) / "assets"
    for name in sorted(asset_names(identity["version"])):
        command(["gh", "api", "--method", "POST", "--header", "Content-Type: application/octet-stream",
                 "--header", f"Content-Length: {(assets / name).stat().st_size}",
                 f"https://uploads.github.com/repos/{repository}/releases/{release_id}/assets?name={name}",
                 "--input", str(assets / name)], operation="GitHub asset upload")
    release = api(repository, f"releases/{release_id}", operation="GitHub draft validation")
    require(release.get("draft") is True and release.get("tag_name") == identity["tag"]
            and release.get("prerelease") is False and release.get("id") == release_id,
            "Unexpected draft release")
    remote_assets = api(repository, f"releases/{release_id}/assets?per_page=100", "--paginate", "--slurp",
                        operation="GitHub asset validation")
    require(isinstance(remote_assets, list) and all(isinstance(p, list) for p in remote_assets), "Invalid asset response")
    rows = [row for page in remote_assets for row in page]
    names = asset_names(identity["version"])
    require(len(rows) == 6 and {row.get("name") for row in rows} == names, "Uploaded assets differ")
    for row in rows:
        path = assets / row["name"]
        require(row.get("state") == "uploaded" and row.get("size") == path.stat().st_size
                and row.get("digest") == "sha256:" + digest(path), "Uploaded asset verification failed")
    require(check_blockers.check(repository) == 0, "Open release blockers")
    require_remote(identity)
    api(repository, f"releases/{release_id}", "--method", "PATCH", "-F", "draft=false",
        operation="GitHub release publication")
    published = api(repository, f"releases/{release_id}", operation="GitHub publication validation")
    require(published.get("id") == release_id and published.get("tag_name") == identity["tag"]
            and published.get("draft") is False and published.get("immutable") is True,
            "Publication state uncertain; inspect GitHub before manual recovery")


def require_remote(identity):
    refs = remote_refs()
    require(refs.get("refs/heads/main") == identity["development"]
            and refs.get("refs/tags/" + identity["tag"]) == identity["tag_object"], "Published refs changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    stages = parser.add_subparsers(dest="operation", required=True)
    for operation in ("plan", "materialize", "recheck", "prepare", "verify", "publish"):
        stage = stages.add_parser(operation)
        if operation in ("plan", "materialize", "recheck", "prepare"):
            stage.add_argument("--plan", required=True)
        if operation in ("prepare", "verify", "publish"):
            stage.add_argument("--output", required=True)
        if operation in ("plan", "recheck", "prepare", "publish"):
            stage.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", "scylladb/alternator-client-cpp"))
        if operation == "plan":
            stage.add_argument("--version", default="")
            stage.add_argument("--baseline", required=True)
            stage.add_argument("--tree")
    args = parser.parse_args()
    try:
        if args.operation == "plan":
            plan = plan_release(args.version, args.baseline)
            if args.tree:
                require(plan["tree"] == args.tree, "Validated release tree changed")
            available(plan, args.repo)
            Path(args.plan).parent.mkdir(parents=True, exist_ok=True)
            write_json(args.plan, plan)
            for key in ("baseline", "version", "tree"):
                print(f"{key}={plan[key]}")
        elif args.operation in ("materialize", "recheck", "prepare"):
            plan = read_plan(args.plan)
            if args.operation == "materialize":
                require(oid("HEAD") == plan["baseline"] and not git("status", "--porcelain").strip(),
                        "Materialization requires a clean baseline checkout and an external plan")
                git("read-tree", "--reset", "-u", plan["tree"])
            elif args.operation == "recheck":
                available(plan, args.repo)
            else:
                prepare_signed(plan, args.output, args.repo)
        elif args.operation == "verify":
            verify_recovery(args.output)
        else:
            publish(args.output, args.repo)
        return 0
    except (ReleaseError, signing.SigningError) as error:
        print(str(error), file=sys.stderr)
        return 1
    except Exception as error:
        print(f"Release lifecycle {args.operation} failed ({type(error).__name__}); values withheld. "
              "Do not rerun after remote changes; follow recovery instructions.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
