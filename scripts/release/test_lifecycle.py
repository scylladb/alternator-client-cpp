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

from contextlib import redirect_stderr
from io import StringIO
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import lifecycle
import signing
import validate
import test_signing

ROOT = Path(os.environ.get("RELEASE_LAUNCH_ROOT", Path(__file__).resolve().parents[2]))


class VersionTests(unittest.TestCase):
    def test_stable_versions_and_next_patch(self):
        self.assertEqual(lifecycle.next_version("2.3.99"), "2.3.100-SNAPSHOT")
        for value in ("", "01.2.3", "1.2", "v1.2.3", "1.2.3-rc.1", "1.2.3-SNAPSHOT",
                      "1.2.3\n", "1.2.3;exit", "$(false)", None):
            with self.subTest(value=value), self.assertRaises(lifecycle.ReleaseError):
                lifecycle.version(value)

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_uses_numeric_package_and_complete_macro_versions(self):
        with tempfile.TemporaryDirectory(prefix="release-cmake-version-") as temp:
            build = Path(temp) / "build"
            result = subprocess.run(["cmake", "-S", str(ROOT), "-B", str(build),
                "-DALTERNATOR_CLIENT_CPP_ENABLE_AWS=OFF", "-DALTERNATOR_CLIENT_CPP_BUILD_TESTS=OFF",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            full = (ROOT / "VERSION").read_text().strip()
            numeric = full.removesuffix("-SNAPSHOT")
            self.assertIn("CMAKE_PROJECT_VERSION:STATIC=" + numeric, (build / "CMakeCache.txt").read_text())
            commands = json.loads((build / "compile_commands.json").read_text())
            self.assertTrue(all(full in row["command"] for row in commands))
            self.assertIn('set(PACKAGE_VERSION "' + numeric + '")',
                          (build / "scylladb-alternator-client-cpp-config-version.cmake").read_text())

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_rejects_malformed_version_before_configuring_source(self):
        with tempfile.TemporaryDirectory(prefix="release-cmake-invalid-") as temp:
            source = Path(temp)
            shutil.copyfile(ROOT / "CMakeLists.txt", source / "CMakeLists.txt")
            for value in ("1.2.3-rc.1", "01.2.3", "1.2.3;message(FATAL_ERROR injected)"):
                (source / "VERSION").write_text(value)
                result = subprocess.run(["cmake", "-S", str(source), "-B", str(source / "build")],
                                        capture_output=True, text=True, timeout=60)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("VERSION must contain", result.stderr)


class ApiTests(unittest.TestCase):
    def test_paginated_release_lookup_includes_later_drafts(self):
        pages = [[{"tag_name": "v0.0.1"}], [{"tag_name": "v1.2.3", "draft": True}]]
        with patch.object(lifecycle, "api", return_value=pages) as api:
            self.assertEqual(len(lifecycle.releases("owner/repo")), 2)
            self.assertIn("--paginate", api.call_args.args)
            self.assertIn("--slurp", api.call_args.args)
        for response in (None, [], [{}], [[{"tag_name": None}]]):
            with patch.object(lifecycle, "api", return_value=response), self.assertRaises(lifecycle.ReleaseError):
                lifecycle.releases("owner/repo")


class CommandTests(unittest.TestCase):
    def test_cli_rejects_missing_stage_inputs_before_starting_commands(self):
        cases = [
            ["plan", "--plan", "plan.json"],
            ["plan", "--baseline", "0" * 40],
            ["materialize"], ["recheck"],
            ["prepare", "--plan", "plan.json"],
            ["prepare", "--output", "recovery"],
            ["verify"], ["publish"],
        ]
        for args in cases:
            with self.subTest(args=args), patch.object(sys, "argv", ["lifecycle.py", *args]), \
                    patch.object(lifecycle, "command") as command, redirect_stderr(StringIO()):
                with self.assertRaises(SystemExit) as error:
                    lifecycle.main()
                self.assertEqual(error.exception.code, 2)
                command.assert_not_called()

    def test_failures_name_safe_operations_without_command_values(self):
        secret = "credential-bearing-value"
        with patch.object(lifecycle.subprocess, "run", return_value=subprocess.CompletedProcess([], 7, b"", secret)):
            with self.assertRaises(lifecycle.ReleaseError) as error:
                lifecycle.git("push", "origin", secret)
        self.assertIn("Git push failed (exit status 7)", str(error.exception))
        self.assertNotIn(secret, str(error.exception))

    def test_timeouts_and_startup_errors_do_not_expose_exception_values(self):
        secret = "credential-bearing-value"
        for failure, expected in ((subprocess.TimeoutExpired([secret], 180, stderr=secret), "timed out"),
                                  (OSError(secret), "could not start")):
            with self.subTest(failure=type(failure).__name__), patch.object(lifecycle.subprocess, "run", side_effect=failure):
                with self.assertRaises(lifecycle.ReleaseError) as error:
                    lifecycle.command(["gh", "api", secret], operation="GitHub asset upload")
                self.assertIn("GitHub asset upload " + expected, str(error.exception))
                self.assertNotIn(secret, str(error.exception))

    def test_unexpected_cli_error_names_operation_and_type_without_values(self):
        stderr = StringIO()
        with patch.object(sys, "argv", ["lifecycle.py", "publish", "--output", "recovery"]), \
                patch.object(lifecycle, "publish", side_effect=RuntimeError("credential-bearing-value")), \
                redirect_stderr(stderr):
            self.assertEqual(lifecycle.main(), 1)
        self.assertIn("publish failed (RuntimeError)", stderr.getvalue())
        self.assertNotIn("credential-bearing-value", stderr.getvalue())



@unittest.skipUnless(shutil.which("gpg") and shutil.which("git"), "GnuPG and Git required")
class LifecycleTests(unittest.TestCase):
    setUpClass = classmethod(test_signing.SigningTests.setUpClass.__func__)
    tearDownClass = classmethod(test_signing.SigningTests.tearDownClass.__func__)
    repository = test_signing.SigningTests.repository
    session = test_signing.SigningTests.session

    def source(self, value="0.1.0-SNAPSHOT"):
        Path("VERSION").write_text(value + "\n")
        Path("README.md").write_text("source documentation\n")
        Path("LICENSE").write_text("license\n")
        signing.run(["git", "add", "."])
        signing.run(["git", "-c", "user.name=Initial", "-c", "user.email=initial@example.invalid",
                     "-c", "commit.gpgsign=false", "commit", "-qm", "Source"])
        os.environ["GITHUB_ACTIONS"] = "true"
        os.environ["GITHUB_REF"] = "refs/heads/main"
        return lifecycle.plan_release()

    def remote(self, directory, baseline):
        remote = directory / "remote.git"
        lifecycle.git("init", "--bare", str(remote))
        lifecycle.git("remote", "add", "origin", str(remote))
        lifecycle.git("push", "origin", baseline + ":refs/heads/main")
        return remote

    def prepared(self, directory):
        plan = self.source()
        self.remote(directory, plan["baseline"])
        output = directory / "recovery"
        with self.session():
            identity = lifecycle.prepare(plan, output)
        return identity, output

    def test_defaults_override_and_input_boundaries(self):
        with self.repository():
            plan = self.source("1.2.9-SNAPSHOT")
            self.assertEqual(plan["version"], "1.2.9")
            self.assertEqual(lifecycle.plan_release("1.2.10")["version"], "1.2.10")
            self.assertEqual(lifecycle.plan_release("2.0.0")["version"], "2.0.0")
            for value in ("1.2.8", "1.2.9-rc.1", "1.2.09"):
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.plan_release(value)
            with patch.dict(os.environ, {"GITHUB_REF": "refs/tags/v1.2.9"}):
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.plan_release()
            with patch.dict(os.environ, {"GITHUB_REF": ""}):
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.plan_release()
            lifecycle.git("-c", "tag.gpgSign=false", "tag", "v1.2.9")
            with self.assertRaises(lifecycle.ReleaseError):
                lifecycle.plan_release()

    def test_stable_version_file_defaults(self):
        with self.repository():
            self.assertEqual(self.source("4.5.6")["version"], "4.5.6")

    def test_signed_preparation_exact_history_and_six_assets(self):
        with self.repository() as directory:
            identity, output = self.prepared(directory)
            self.assertEqual(lifecycle.verify_recovery(output), identity)
            self.assertEqual(lifecycle.oid("HEAD"), identity["baseline"])
            self.assertEqual(lifecycle.git("show", identity["release"] + ":VERSION"), b"0.1.0\n")
            self.assertEqual(lifecycle.git("show", identity["development"] + ":VERSION"), b"0.1.1-SNAPSHOT\n")
            self.assertEqual({p.name for p in (output / "assets").iterdir()}, lifecycle.asset_names("0.1.0"))
            self.assertEqual(lifecycle.remote_refs(), {"refs/heads/main": identity["baseline"]})
            self.assertFalse(list(directory.glob("release-signing-*")))
            for path in output.rglob("*"):
                if path.is_file():
                    self.assertNotIn(self.password.encode(), path.read_bytes())
                    self.assertNotIn(b"BEGIN PGP PRIVATE KEY", path.read_bytes())
            with self.session(), self.assertRaises(lifecycle.ReleaseError):
                lifecycle.prepare({k: identity[k] for k in ("baseline", "version", "tag", "tree")}, directory / "retry")

    def test_recovery_bundle_import_preserves_exact_objects(self):
        with self.repository() as directory:
            identity, output = self.prepared(directory)
            recovered = directory / "recovered"
            lifecycle.git("clone", str(output / "history.bundle"), str(recovered))
            original = Path.cwd()
            try:
                os.chdir(recovered)
                lifecycle.verify_recovery(output)
                self.assertEqual(lifecycle.oid("refs/tags/" + identity["tag"]), identity["tag_object"])
            finally:
                os.chdir(original)

    def test_cli_preparation_uses_real_isolated_session_and_leaves_remote_unchanged(self):
        with self.repository() as directory:
            for source in (ROOT / "scripts/release").glob("*.py"):
                target = Path("scripts/release") / source.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            helper = Path("scripts/release/signing.py")
            helper.write_text(helper.read_text().replace(test_signing.REPOSITORY_PRIMARY, self.primary)
                              .replace(test_signing.REPOSITORY_SIGNING, self.subkey))
            shutil.copyfile(self.public, "scripts/release/publisher.asc")
            plan = self.source()
            self.remote(directory, plan["baseline"])
            binaries = directory / "bin"
            binaries.mkdir()
            gh = binaries / "gh"
            gh.write_text("#!/bin/sh\ncase \"$*\" in *releases?per_page=100*|*issues?state=open*) printf '[[]]';; *) exit 99;; esac\n")
            gh.chmod(0o700)
            plan_path = directory / "plan.json"
            output = directory / "recovery"
            env = {**os.environ, "PATH": str(binaries) + os.pathsep + os.environ["PATH"]}
            signing.run(["python3", "scripts/release/lifecycle.py", "plan", "--baseline", plan["baseline"],
                         "--version", "1.2.3", "--plan", str(plan_path)], env=env)
            signing.run(["python3", "scripts/release/guard.py", "run", "python3", "scripts/release/lifecycle.py",
                         "prepare", "--plan", str(plan_path), "--output", str(output)], env=env)
            signing.run(["python3", "scripts/release/lifecycle.py", "verify", "--output", str(output)], env=env)
            self.assertEqual(lifecycle.verify_recovery(output)["version"], "1.2.3")
            self.assertEqual(lifecycle.remote_refs(), {"refs/heads/main": plan["baseline"]})
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_advancing_main_existing_tags_and_drafts_fail_closed(self):
        with self.repository() as directory:
            plan = self.source()
            remote = self.remote(directory, plan["baseline"])
            with patch.object(lifecycle, "releases", return_value=[]):
                lifecycle.available(plan, "owner/repo")
            with patch.object(lifecycle, "releases", return_value=[{"tag_name": plan["tag"], "draft": True}]):
                with self.assertRaisesRegex(lifecycle.ReleaseError, "draft"):
                    lifecycle.available(plan, "owner/repo")
            with patch.object(lifecycle, "releases", side_effect=RuntimeError("API failed")):
                with self.assertRaises(RuntimeError):
                    lifecycle.available(plan, "owner/repo")
            lifecycle.git("--git-dir=" + str(remote), "update-ref", "refs/tags/" + plan["tag"], plan["baseline"])
            with self.assertRaisesRegex(lifecycle.ReleaseError, "tag already exists"):
                lifecycle.available(plan, "owner/repo")
            lifecycle.git("--git-dir=" + str(remote), "update-ref", "-d", "refs/tags/" + plan["tag"])
            changed = lifecycle.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                                    "commit-tree", plan["tree"], "-p", plan["baseline"], "-m", "Advanced").decode().strip()
            lifecycle.git("push", "origin", changed + ":refs/heads/main")
            with self.assertRaisesRegex(lifecycle.ReleaseError, "main advanced"):
                lifecycle.available(plan, "owner/repo")

    def test_unexpected_tree_parent_or_signature_is_rejected(self):
        with self.repository() as directory:
            identity, _ = self.prepared(directory)
            with self.session():
                bad_tree = lifecycle.version_tree(identity["release"], "9.9.9")
                bad = lifecycle.git("commit-tree", "-S", bad_tree, "-p", identity["release"], "-m", "Wrong version").decode().strip()
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.verify_history({**identity, "development": bad})
                tree = lifecycle.oid(identity["development"] + "^{tree}")
                bad = lifecycle.git("commit-tree", "-S", tree, "-p", identity["baseline"], "-m", "Wrong parent").decode().strip()
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.verify_history({**identity, "development": bad})
                unsigned = lifecycle.git("commit-tree", tree, "-p", identity["release"], "-m", "Unsigned").decode().strip()
                with self.assertRaises(signing.SigningError):
                    lifecycle.verify_history({**identity, "development": unsigned})

    def test_signed_extra_source_change_is_rejected_in_coherent_history(self):
        with self.repository() as directory:
            identity, _ = self.prepared(directory)
            with self.session():
                # Keep the correct release VERSION and change only another source file.
                env = {**os.environ, "GIT_INDEX_FILE": str(directory / "extra-source-index")}
                lifecycle.git("read-tree", identity["release"], env=env)
                blob = lifecycle.git("hash-object", "-w", "--stdin", data=b"unexpected change\n").decode().strip()
                lifecycle.git("update-index", "--cacheinfo", "100644", blob, "README.md", env=env)
                tree = lifecycle.git("write-tree", env=env).decode().strip()
                release = lifecycle.git("commit-tree", "-S", tree, "-p", identity["baseline"],
                                        "-m", "Extra source").decode().strip()
                development = lifecycle.git("commit-tree", "-S",
                    lifecycle.version_tree(release, lifecycle.next_version(identity["version"])),
                    "-p", release, "-m", "Begin development").decode().strip()
                lifecycle.git("tag", "-f", "-s", "-m", "Release", identity["tag"], release)
                changed = {**identity, "release": release, "development": development,
                           "tag_object": lifecycle.oid("refs/tags/" + identity["tag"])}
                with self.assertRaisesRegex(lifecycle.ReleaseError, "changes files beyond VERSION"):
                    lifecycle.verify_history(changed)

    def test_asset_and_manifest_tampering_are_rejected(self):
        with self.repository() as directory:
            _, output = self.prepared(directory)
            manifest = output / "manifest.json"
            original = manifest.read_bytes()
            manifest.write_bytes(original + b" ")
            with self.assertRaises(signing.SigningError):
                lifecycle.verify_recovery(output)
            manifest.write_bytes(original)
            archive = next((output / "assets").glob("*.zip"))
            archive.write_bytes(archive.read_bytes() + b"tampered")
            with self.assertRaisesRegex(lifecycle.ReleaseError, "hash mismatch"):
                lifecycle.verify_recovery(output)

    def test_atomic_push_rejection_leaves_both_remote_refs_unchanged(self):
        with self.repository() as directory:
            identity, output = self.prepared(directory)
            hook = directory / "remote.git/hooks/pre-receive"
            hook.write_text("#!/bin/sh\nwhile read old new ref; do\ncase $ref in refs/tags/*) exit 1;; esac\ndone\n")
            hook.chmod(0o700)
            with patch.object(lifecycle, "releases", return_value=[]), \
                    patch.object(lifecycle.check_blockers, "check", return_value=0):
                with self.assertRaises(lifecycle.ReleaseError):
                    lifecycle.publish(output, "owner/repo")
            self.assertEqual(lifecycle.remote_refs(), {"refs/heads/main": identity["baseline"]})

    def test_draft_first_publication_and_upload_failures(self):
        for failure in ("upload", "replaced", "digest", "blocker", "api", None):
            with self.subTest(failure=failure), self.repository() as directory:
                identity, output = self.prepared(directory)
                lifecycle.git("-c", "tag.gpgSign=false", "-c", "user.name=Local",
                              "-c", "user.email=local@example.invalid", "tag", "-a", "unrelated-local-tag",
                              identity["baseline"], "-m", "Unrelated local tag")
                lifecycle.git("config", "push.followTags", "true")
                events = []
                uploaded_to = []
                real_command = lifecycle.command
                def command(args, **kwargs):
                    if args[0] != "gh":
                        return real_command(args, **kwargs)
                    events.append(args)
                    if args[:2] == ["gh", "api"] and "--input" in args:
                        url = next(arg for arg in args if arg.startswith("https://uploads.github.com/"))
                        self.assertIn("/releases/123/assets?name=", url)
                        self.assertIn("Content-Type: application/octet-stream", args)
                        self.assertIn(f"Content-Length: {Path(args[args.index('--input') + 1]).stat().st_size}", args)
                        self.assertEqual(Path(args[args.index("--input") + 1]).name, url.split("?name=")[1])
                        if failure == "upload":
                            raise lifecycle.ReleaseError("Pinned upload failed")
                        uploaded_to.append(123)
                    return b""
                def api(repository, endpoint, *args, **kwargs):
                    events.append([endpoint, *args])
                    if endpoint == "releases/123" and not args:
                        published = any("PATCH" in row for row in events)
                        return {"id": 456 if failure == "replaced" else 123,
                                "tag_name": identity["tag"], "draft": not published,
                                "immutable": published, "prerelease": False}
                    if endpoint.endswith("assets?per_page=100"):
                        if failure == "api":
                            raise lifecycle.ReleaseError("API unavailable")
                        return [[{"name": p.name, "size": p.stat().st_size, "state": "uploaded",
                                  "digest": "sha256:" + ("0" * 64 if failure == "digest" else lifecycle.digest(p))}
                                 for p in (output / "assets").iterdir()]]
                    return {}
                def release_listing(repository):
                    if any("create" in row for row in events):
                        return [{"id": 123, "tag_name": identity["tag"], "draft": True, "prerelease": False}]
                    return []
                with patch.object(lifecycle, "command", side_effect=command), \
                        patch.object(lifecycle, "api", side_effect=api), \
                        patch.object(lifecycle, "releases", side_effect=release_listing), \
                        patch.object(lifecycle.check_blockers, "check", side_effect=[0, 1 if failure == "blocker" else 0]):
                    if failure == "replaced":
                        with self.assertRaisesRegex(lifecycle.ReleaseError, "Unexpected draft release"):
                            lifecycle.publish(output, "owner/repo")
                    elif failure:
                        with self.assertRaises(lifecycle.ReleaseError):
                            lifecycle.publish(output, "owner/repo")
                    else:
                        lifecycle.publish(output, "owner/repo")
                self.assertEqual(lifecycle.remote_refs(), {"refs/heads/main": identity["development"],
                                 "refs/tags/" + identity["tag"]: identity["tag_object"]})
                self.assertIn("--draft", next(row for row in events if "create" in row))
                self.assertEqual(any("PATCH" in row for row in events), failure is None)
                self.assertFalse(any("--clobber" in row for row in events))
                self.assertEqual(uploaded_to, [] if failure == "upload" else [123] * 6)


class ValidationTests(unittest.TestCase):
    def test_skip_policy_always_builds_lints_and_checks_packages(self):
        plan = {"baseline": "0" * 40, "tree": "1" * 40}
        for skip in (False, True):
            calls = []
            env = {k: v for k, v in os.environ.items() if not k.startswith(("RELEASE_GPG_", "GPG_"))}
            with patch.dict(os.environ, env, clear=True), patch.object(lifecycle, "read_plan", return_value=plan), \
                    patch.object(lifecycle, "git"), patch.object(subprocess, "run", side_effect=lambda args, **kw: calls.append(args)):
                validate.validate("plan.json", skip)
            flattened = [" ".join(map(str, args)) for args in calls]
            self.assertTrue(any("unittest discover" in cmd for cmd in flattened))
            for mandatory in ("make build", "make check", "package_check.py"):
                self.assertTrue(any(mandatory in cmd for cmd in flattened))
            for test in ("make test-unit", "make test-integration"):
                self.assertEqual(any(test in cmd for cmd in flattened), not skip)

    def test_validation_rejects_signing_credentials_before_children(self):
        with patch.object(lifecycle, "read_plan", return_value={}), \
                patch.dict(os.environ, {"RELEASE_GPG_PASSPHRASE": "do-not-disclose"}), \
                patch.object(subprocess, "run") as child:
            with self.assertRaises(lifecycle.ReleaseError):
                validate.validate("plan.json")
            child.assert_not_called()

    def test_workflows_share_actions_and_gate_remote_changes_on_recovery(self):
        for dry in (False, True):
            workflow = (ROOT / (".github/workflows/release" + ("-dry-run" if dry else "") + ".yml")).read_text()
            self.assertNotIn("target-tag", workflow)
            self.assertNotIn("  push:", workflow)
            validation, release = workflow.split("  release:\n", 1)
            self.assertNotIn("secrets.", validation)
            self.assertNotIn("environment: release", validation)
            self.assertIn("    needs: validate", release)
            self.assertIn("    environment: release", release)
            self.assertIn("      cancel-in-progress: false", release)
            self.assertIn("group: alternator-client-cpp-release", release)
            self.assertIn("uses: ./.github/actions/release-validate", validation)
            self.assertIn("uses: ./.github/actions/release-prepare", release)
            self.assertNotIn("actions/cache", release)
            if dry:
                self.assertNotIn("skip-tests:", workflow.split("permissions:", 1)[0])
                self.assertNotIn("lifecycle.py\" publish", release)
                self.assertIn("          skip-tests: 'false'", validation)
                self.assertIn("      contents: read", release)
            else:
                self.assertLess(release.index("Retain exact recovery"), release.index("Atomically push"))
                self.assertIn("Check blockers before validation", validation)
                self.assertIn("Check blockers after validation", validation)
                self.assertIn("Check blockers after approval", release)


if __name__ == "__main__":
    unittest.main()
