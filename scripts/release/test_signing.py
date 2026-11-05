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

"""Exercise release signing with disposable protected keys and real Git objects."""

from contextlib import contextmanager, redirect_stderr
from io import StringIO
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import signing

ROOT = Path(os.environ.get("RELEASE_LAUNCH_ROOT", Path(__file__).resolve().parents[2]))
REPOSITORY_PRIMARY = signing.PRIMARY
REPOSITORY_SIGNING = signing.SIGNING


@unittest.skipUnless(shutil.which("gpg") and shutil.which("git"), "GnuPG and Git required")
class SigningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory(prefix="repo-signing-fixture-")
        cls.directory = Path(cls.fixture.name)
        cls.keyring = cls.directory / "gnupg"
        cls.keyring.mkdir(mode=0o700)
        cls.password = "test-only-release-passphrase"
        def gpg(*args, home=cls.keyring, password=cls.password):
            result = subprocess.run(
                ["gpg", "--no-options", "--homedir", str(home), "--batch",
                 "--pinentry-mode", "loopback", "--passphrase-fd", "0", *args],
                input=password + "\n", capture_output=True, text=True, check=True, timeout=60,
            )
            return result.stdout
        gpg("--quick-generate-key", "Release Test <release@example.invalid>", "rsa2048", "cert", "1d")
        cls.primary = [line.split(":")[9] for line in
                       gpg("--with-colons", "--list-keys").splitlines() if line.startswith("fpr:")][0]
        gpg("--quick-add-key", cls.primary, "rsa2048", "sign", "1d")
        cls.subkey = [line.split(":")[9] for line in
                      gpg("--with-colons", "--list-keys").splitlines() if line.startswith("fpr:")][1]
        cls.public = cls.directory / "public.asc"
        cls.public.write_text(gpg("--armor", "--export", cls.primary))
        cls.private = cls.directory / "subkey.asc"
        cls.private.write_text(gpg("--armor", "--export-secret-subkeys", cls.primary))
        cls.full = cls.directory / "full.asc"
        cls.full.write_text(gpg("--armor", "--export-secret-keys", cls.primary))
        cls.unprotected_home = cls.directory / "unprotected"
        cls.unprotected_home.mkdir(mode=0o700)
        options = {"home": cls.unprotected_home, "password": ""}
        gpg("--quick-generate-key", "Other Test <other@example.invalid>", "rsa2048", "cert", "1d", **options)
        cls.unprotected_primary = [line.split(":")[9] for line in
                                   gpg("--with-colons", "--list-keys", **options).splitlines()
                                   if line.startswith("fpr:")][0]
        gpg("--quick-add-key", cls.unprotected_primary, "rsa2048", "sign", "1d", **options)
        cls.unprotected_subkey = [line.split(":")[9] for line in
                                  gpg("--with-colons", "--list-keys", **options).splitlines()
                                  if line.startswith("fpr:")][1]
        cls.unprotected_public = cls.directory / "unprotected-public.asc"
        cls.unprotected_public.write_text(gpg("--armor", "--export", cls.unprotected_primary, **options))
        cls.unprotected_private = cls.directory / "unprotected-private.asc"
        cls.unprotected_private.write_text(gpg("--armor", "--export-secret-subkeys",
                                               cls.unprotected_primary, **options))

    @classmethod
    def tearDownClass(cls):
        try:
            subprocess.run(["gpgconf", "--homedir", str(cls.keyring), "--kill", "gpg-agent"],
                           capture_output=True, check=True, timeout=60)
        finally:
            try:
                subprocess.run(["gpgconf", "--homedir", str(cls.unprotected_home), "--kill", "gpg-agent"],
                               capture_output=True, check=True, timeout=60)
            finally:
                cls.fixture.cleanup()

    @contextmanager
    def repository(self, full=False, password=None):
        original = Path.cwd()
        with tempfile.TemporaryDirectory(prefix="repo-signing-test-") as temp:
            directory = Path(temp)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("GIT_", "GITHUB_", "RELEASE_", "MAKE"))}
            env.update(RUNNER_TEMP=str(directory), GITHUB_ENV=str(directory / "github-env"),
                       GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=str(directory / "gitconfig"),
                       RELEASE_GPG_PRIVATE_KEY_FILE=str(self.full if full else self.private),
                       RELEASE_GPG_PASSPHRASE=self.password if password is None else password)
            Path(env["GIT_CONFIG_GLOBAL"]).write_text("")
            with patch.dict(os.environ, env, clear=True), \
                    patch.multiple(signing, PRIMARY=self.primary, SIGNING=self.subkey,
                                   PUBLIC_KEY=self.public):
                repo = directory / "source"
                signing.run(["git", "init", "--quiet", str(repo)])
                os.chdir(repo)
                try:
                    signing.run(["git", "-c", "user.name=Initial", "-c",
                                 "user.email=initial@example.invalid", "-c", "commit.gpgsign=false",
                                 "commit", "--quiet", "--allow-empty", "-m", "Initial"])
                    yield directory
                finally:
                    os.chdir(original)

    @contextmanager
    def session(self):
        with signing.signing_session() as env, patch.dict(os.environ, env, clear=True):
            self.assertNotIn("RELEASE_GPG_PRIVATE_KEY", env)
            self.assertNotIn("RELEASE_GPG_PRIVATE_KEY_FILE", env)
            yield env

    def release(self):
        signing.run(["git", "commit", "--quiet", "--allow-empty", "-m", "Release"])
        signing.run(["git", "tag", "-s", "-m", "Release 1.0", "v1.0.0"])

    def test_repository_setup_failure_restores_working_directory(self):
        original = Path.cwd()
        real_run = signing.run
        def fail_commit(command, env=None):
            if "commit" in command:
                raise signing.SigningError("fixture commit failed")
            return real_run(command, env)
        with patch.object(signing, "run", side_effect=fail_commit):
            with self.assertRaisesRegex(signing.SigningError, "fixture commit failed"):
                with self.repository():
                    self.fail("Initial commit failure accepted")
        self.assertEqual(Path.cwd(), original)
        with self.repository():
            self.assertTrue(Path(".git").is_dir())

    def test_signed_commit_and_tag_verify_after_session(self):
        with self.repository() as directory:
            ambient = directory / "ambient-keyring"
            ambient.mkdir(mode=0o700)
            (ambient / "marker").write_text("unchanged")
            os.environ["GNUPGHOME"] = str(ambient)
            # Existing checkout settings must not override the session.
            signing.run(["git", "config", "user.signingkey", "invalid-key"])
            config = Path(".git/config").read_bytes()
            with self.session() as env:
                self.assertNotEqual(env["GNUPGHOME"], str(ambient))
                self.release()
            signing.verify_commit("HEAD")
            signing.verify_tag("v1.0.0", signing.run(["git", "rev-parse", "HEAD"]).strip())
            self.assertEqual(Path(".git/config").read_bytes(), config)
            self.assertFalse(list(directory.glob("release-signing-*")))
            self.assertFalse(Path(os.environ["GITHUB_ENV"]).exists())
            self.assertEqual((ambient / "marker").read_text(), "unchanged")
            self.assertEqual(list(ambient.iterdir()), [ambient / "marker"])

    def test_missing_export_does_not_use_ambient_keyring(self):
        with self.repository() as directory:
            os.environ["GNUPGHOME"] = str(self.keyring)
            del os.environ["RELEASE_GPG_PRIVATE_KEY_FILE"]
            with self.assertRaisesRegex(signing.SigningError, "signing-subkey export"), self.session():
                self.fail("Missing export accepted")
            self.assertEqual(os.environ["GNUPGHOME"], str(self.keyring))
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_unprotected_export_rejected(self):
        with self.repository() as directory, \
                patch.multiple(signing, PRIMARY=self.unprotected_primary, SIGNING=self.unprotected_subkey,
                               PUBLIC_KEY=self.unprotected_public):
            os.environ["RELEASE_GPG_PRIVATE_KEY_FILE"] = str(self.unprotected_private)
            with self.assertRaisesRegex(signing.SigningError, "protected subkey"), self.session():
                self.fail("Unprotected export accepted")
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_empty_passphrase_fails_before_configuration(self):
        with self.repository(password="") as directory:
            with self.assertRaisesRegex(signing.SigningError, "nonempty"), self.session():
                self.fail("Empty passphrase accepted")
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_wrong_passphrase_fails_with_fresh_agent(self):
        with self.repository(password="wrong-test-passphrase") as directory:
            with self.assertRaises(signing.SigningError), self.session():
                self.fail("Wrong passphrase accepted")
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_primary_secret_rejected(self):
        with self.repository(full=True):
            with self.assertRaisesRegex(signing.SigningError, "signing-private.asc"), self.session():
                self.fail("Primary private key accepted")

    def test_unsigned_commit_and_tag_rejected(self):
        with self.repository():
            with self.session():
                signing.run(["git", "-c", "commit.gpgsign=false", "commit", "--quiet",
                             "--allow-empty", "-m", "Unsigned"])
                signing.run(["git", "-c", "tag.gpgSign=false", "tag", "-a", "-m",
                             "Unsigned", "v1.0.0"])
            with self.assertRaises(signing.SigningError):
                signing.verify_commit("HEAD")
            with self.assertRaises(signing.SigningError):
                signing.verify_tag("v1.0.0")

    def test_tag_at_unexpected_commit_rejected(self):
        with self.repository():
            with self.session():
                self.release()
                signing.run(["git", "commit", "--quiet", "--allow-empty", "-m", "Later"])
            with self.assertRaisesRegex(signing.SigningError, "unexpected commit"):
                signing.verify_tag("v1.0.0", signing.run(["git", "rev-parse", "HEAD"]).strip())

    def test_public_tag_verification_requires_commit_resolution(self):
        with self.repository():
            blob = signing.run(["git", "hash-object", "-w", "--stdin"], input="artifact").strip()
            with self.session():
                for kind, target in [("tree", "HEAD^{tree}"), ("blob", blob)]:
                    signing.run(["git", "tag", "-s", "-m", kind, "v1.0.0-" + kind, target])
                self.release()
                signing.run(["git", "tag", "-s", "-m", "Nested", "v1.0.1", "v1.0.0"])
            for kind in ["tree", "blob"]:
                with self.subTest(kind=kind), self.assertRaises(signing.SigningError):
                    signing.verify_tag("v1.0.0-" + kind)
            signing.verify_tag("v1.0.1")
            signing.verify_tag("v1.0.1", signing.run(["git", "rev-parse", "HEAD"]).strip())

    def test_renamed_signed_tag_rejected(self):
        with self.repository(), self.session():
            self.release()
            signing.run(["git", "update-ref", "refs/tags/v9.9.9", "refs/tags/v1.0.0"])
            with self.assertRaisesRegex(signing.SigningError, "name"):
                signing.verify_tag("v9.9.9")

    def test_tag_signed_by_other_key_rejected(self):
        with self.repository():
            env = {**os.environ, "GNUPGHOME": str(self.unprotected_home)}
            signing.run(["git", "-c", "user.name=Other", "-c", "user.email=other@example.invalid",
                         "-c", "gpg.format=openpgp", "-c", "gpg.program=gpg",
                         "-c", "user.signingkey=" + self.unprotected_subkey + "!",
                         "tag", "-s", "-m", "Other signer", "v1.0.0"], env=env)
            with self.assertRaises(signing.SigningError):
                signing.verify_tag("v1.0.0")

    def test_batch_signing_reuse_and_tamper_rejection(self):
        with self.repository(), self.session():
            files = [Path("package.tar.xz"), Path("package.tar.gz"), Path("SHA256SUMS")]
            for artifact in files:
                artifact.write_bytes(b"release package")
            signing.sign_files(files, os.environ)
            signatures = [Path(str(artifact) + ".asc").read_bytes() for artifact in files]
            signing.sign_files(files, os.environ)
            self.assertEqual([Path(str(artifact) + ".asc").read_bytes() for artifact in files], signatures)
            files[0].write_bytes(b"changed package")
            with self.assertRaises(signing.SigningError):
                signing.sign_files(files, os.environ)

    def test_symlink_artifact_sidecar_creation_reuse_and_tamper(self):
        with self.repository() as directory:
            target = directory / "package.tar.gz"
            target.write_bytes(b"package")
            artifact = Path("release.tar.gz")
            artifact.symlink_to(target)
            signature = Path("release.tar.gz.asc")
            signing.sign_files([artifact])
            self.assertTrue(signature.is_file())
            self.assertFalse(Path(str(target) + ".asc").exists())
            previous = signature.read_bytes()
            del os.environ["RELEASE_GPG_PRIVATE_KEY_FILE"]
            del os.environ["RELEASE_GPG_PASSPHRASE"]
            signing.sign_files([artifact])
            self.assertEqual(signature.read_bytes(), previous)
            target.write_bytes(b"tampered package")
            with self.assertRaises(signing.SigningError):
                signing.sign_files([artifact])
            self.assertEqual(signature.read_bytes(), previous)
            self.assertFalse(Path(str(target) + ".asc").exists())

    def test_direct_artifact_signing_uses_session_and_reuses_publicly(self):
        with self.repository() as directory:
            artifact = Path("package.tar.gz")
            artifact.write_bytes(b"package")
            signing.sign_files([artifact])
            signature = Path(str(artifact) + ".asc").read_bytes()
            self.assertFalse(list(directory.glob("release-signing-*")))
            del os.environ["RELEASE_GPG_PRIVATE_KEY_FILE"]
            del os.environ["RELEASE_GPG_PASSPHRASE"]
            signing.sign_files([artifact])
            self.assertEqual(Path(str(artifact) + ".asc").read_bytes(), signature)
            Path(str(artifact) + ".asc").unlink()
            os.environ["GNUPGHOME"] = str(self.keyring)
            with self.assertRaises(signing.SigningError):
                signing.sign_files([artifact])
            self.assertFalse(Path(str(artifact) + ".asc").exists())

    def test_exec_cleans_up_after_command_failure(self):
        with self.repository() as directory:
            config = Path(".git/config").read_bytes()
            with patch("sys.argv", ["signing.py", "exec", sys.executable, "-c", "raise SystemExit(7)"]):
                self.assertEqual(signing.main(), 7)
            self.assertEqual(Path(".git/config").read_bytes(), config)
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_exec_accepts_passphrase_file(self):
        with self.repository() as directory:
            password = directory / "passphrase"
            password.write_text(self.password + "\n")
            os.environ["RELEASE_GPG_PASSPHRASE_FILE"] = str(password)
            del os.environ["RELEASE_GPG_PASSPHRASE"]
            with patch("sys.argv", ["signing.py", "exec", "git", "commit", "--quiet",
                                    "--allow-empty", "-m", "Signed from file credentials"]):
                self.assertEqual(signing.main(), 0)
            signing.verify_commit("HEAD")
            self.assertFalse(list(directory.glob("release-signing-*")))

    def test_cleanup_removes_files_even_when_agent_shutdown_fails(self):
        for status in [0, 7]:
            with self.subTest(status=status), self.repository() as directory:
                real_run = signing.run
                def fail_shutdown(command, *args, **kwargs):
                    result = real_run(command, *args, **kwargs)
                    if command[0] == "gpgconf":
                        # Both public and private cleanup must preserve the result.
                        home = Path(command[command.index("--homedir") + 1])
                        if home.name != "keyring" or (home.parent / "preflight.sig").exists():
                            raise signing.SigningError("DO-NOT-DISCLOSE")
                    return result
                output = StringIO()
                with patch.object(signing, "run", side_effect=fail_shutdown), \
                        patch("sys.argv", ["signing.py", "exec", sys.executable, "-c",
                                           "raise SystemExit(" + str(status) + ")"]), \
                        redirect_stderr(output):
                    self.assertEqual(signing.main(), status)
                self.assertIn("cleanup: GPG agent shutdown failed", output.getvalue())
                self.assertNotIn("DO-NOT-DISCLOSE", output.getvalue())
                self.assertFalse(list(directory.glob("release-signing-*")))

    def test_cancellation_during_import_and_teardown_removes_keyring(self):
        for stage in ["import", "teardown"]:
            with self.subTest(stage=stage), self.repository() as directory:
                original = signal.getsignal(signal.SIGTERM)
                real_import = signing.credential_gpg
                real_stop = signing.stop_agent
                def interrupted_import(*args, **kwargs):
                    result = real_import(*args, **kwargs)
                    os.kill(os.getpid(), signal.SIGTERM)
                    return result
                def interrupted_stop(home):
                    real_stop(home)
                    if Path(home).name == "keyring":
                        os.kill(os.getpid(), signal.SIGTERM)
                with patch.object(signing, "credential_gpg", side_effect=interrupted_import
                                  if stage == "import" else real_import), \
                        patch.object(signing, "stop_agent", side_effect=interrupted_stop
                                     if stage == "teardown" else real_stop):
                    with self.assertRaises(SystemExit) as error:
                        with self.session():
                            self.assertEqual(stage, "teardown")
                self.assertEqual(error.exception.code, 128 + signal.SIGTERM)
                self.assertIs(signal.getsignal(signal.SIGTERM), original)
                self.assertFalse(list(directory.glob("release-signing-*")))

    def test_git_wrapper_reads_current_passphrase_after_agent_reset(self):
        with self.repository(), self.session():
            signing.run(["gpgconf", "--homedir", os.environ["GNUPGHOME"], "--kill", "gpg-agent"])
            os.environ["RELEASE_GPG_PASSPHRASE"] = "incorrect-fixture-passphrase"
            with self.assertRaises(signing.SigningError):
                signing.run(["git", "commit", "--allow-empty", "-qm", "Must fail"])
            os.environ["RELEASE_GPG_PASSPHRASE"] = self.password
            signing.run(["git", "commit", "--allow-empty", "-qm", "Correct passphrase"])
            signing.verify_commit("HEAD")

    def test_other_signer_and_ambiguous_signatures_rejected(self):
        valid = ("[GNUPG:] GOODSIG " + signing.SIGNING[-16:] + " Test signer\n"
                 "[GNUPG:] VALIDSIG " + signing.SIGNING + " 2026-10-20 1 0 4 0 1 10 00 " + signing.PRIMARY)
        signing.verify_status(valid)
        for status in [valid.replace(signing.SIGNING, "0" * 40),
                       valid.replace(signing.PRIMARY, "0" * 40), valid + "\n" + valid,
                       valid.replace(signing.SIGNING[-16:] + " Test", "0" * 16 + " Test"),
                       valid.replace("GOODSIG " + signing.SIGNING[-16:] + " Test signer", "GOODSIG")]:
            with self.assertRaises(signing.SigningError):
                signing.verify_status(status)
        for code in ["REVKEYSIG", "EXPKEYSIG", "KEYREVOKED", "KEYEXPIRED", "EXPSIG", "SIGEXPIRED",
                     "BADSIG", "ERRSIG", "FAILURE", "ERROR"]:
            with self.subTest(code=code), self.assertRaises(signing.SigningError):
                signing.verify_status(valid + "\n[GNUPG:] " + code + " rejected")
        with self.assertRaises(signing.SigningError):
            signing.verify_status(valid.split("\n", 1)[1])

    @contextmanager
    def make_repository(self):
        with self.repository() as directory:
            for name in ["Makefile", "scripts/release/signing.py", "scripts/release/guard.py"]:
                target = Path(name)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text((ROOT / name).read_text())
            helper = Path("scripts/release/signing.py")
            helper.write_text(helper.read_text()
                              .replace(REPOSITORY_PRIMARY, self.primary)
                              .replace(REPOSITORY_SIGNING, self.subkey))
            shutil.copyfile(self.public, "scripts/release/publisher.asc")
            os.environ.update(GITHUB_ACTIONS="true", GITHUB_ACTOR="scylladb-publisher",
                              RELEASE_VERSION="1.0.0")
            yield directory


    def test_recursive_make_does_not_restore_consumed_credentials(self):
        with self.make_repository():
            with Path("Makefile").open("a") as makefile:
                makefile.write('\nprobe:\n\t+@python3 scripts/release/signing.py exec $(MAKE) _probe\n'
                               '_probe:\n\t@python3 probe.py\n')
            Path("probe.py").write_text(
                'import json, os\nfrom pathlib import Path\n'
                'Path("probe.json").write_text(json.dumps(dict(os.environ)))\n')
            private_path = os.environ["RELEASE_GPG_PRIVATE_KEY_FILE"]
            env = {**os.environ, "GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "test.literal",
                   "GIT_CONFIG_VALUE_0": "literal $HOME `whoami`", "RELEASE_GPG_PRIVATE_KEY": "ignored"}
            signing.run(["make", "probe", "RELEASE_GPG_PRIVATE_KEY_FILE=" + private_path,
                         "RELEASE_VERSION=1.2.3-rc.1"], env=env)
            child = json.loads(Path("probe.json").read_text())
            self.assertNotIn("RELEASE_GPG_PRIVATE_KEY", child)
            self.assertNotIn("RELEASE_GPG_PRIVATE_KEY_FILE", child)
            self.assertNotIn(private_path, child.get("MAKEFLAGS", ""))
            self.assertEqual(child["RELEASE_VERSION"], "1.2.3-rc.1")
            self.assertEqual(child["GIT_CONFIG_VALUE_0"], env["GIT_CONFIG_VALUE_0"])
            self.assertEqual(child["GIT_CONFIG_COUNT"], "8")

    def test_exec_stops_children_and_removes_keyring_on_cancellation(self):
        with self.make_repository() as directory:
            state = directory / "child.json"
            # A stubborn grandchild must not outlive cancellation of Make/build.
            child_code = ('import json, os, signal, time; from pathlib import Path; '
                          'signal.signal(signal.SIGTERM, signal.SIG_IGN); '
                          'state = Path(os.environ["CHILD_STATE"]); '
                          'pending = state.with_suffix(".tmp"); '
                          'pending.write_text(json.dumps('
                          '{"pid": os.getpid(), "home": os.environ["GNUPGHOME"]})); '
                          'pending.replace(state); '
                          'time.sleep(60)')
            parent_code = ('import subprocess, sys, time; '
                           'subprocess.Popen([sys.executable, "-c", ' + repr(child_code) + ']); '
                           'time.sleep(60)')
            process = subprocess.Popen([sys.executable, "scripts/release/signing.py", "exec",
                                        sys.executable, "-c", parent_code],
                                       env={**os.environ, "CHILD_STATE": str(state)},
                                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
            info = None
            try:
                deadline = time.monotonic() + 15
                while not state.exists() and process.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertTrue(state.exists(), "Wrapped command never started")
                info = json.loads(state.read_text())
                process.send_signal(signal.SIGTERM)
                _, errors = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 128 + signal.SIGTERM, errors)
                self.assertFalse(Path(info["home"]).exists())
                self.assertFalse(list(directory.glob("release-signing-*")))
                # Linux may retain an orphaned zombie until the runner reaps it.
                proc = Path("/proc") / str(info["pid"]) / "stat"
                if proc.exists():
                    self.assertEqual(proc.read_text().split()[2], "Z")
            finally:
                try:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                finally:
                    if info is not None:
                        try:
                            os.kill(info["pid"], signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.stderr.close()
                    process.wait(timeout=10)


class DiagnosticTests(unittest.TestCase):
    def test_cancellation_during_process_start_stops_owned_group(self):
        original = signal.getsignal(signal.SIGTERM)
        real_popen = subprocess.Popen
        children = []
        def interrupted_start(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            os.kill(os.getpid(), signal.SIGTERM)
            return child
        with patch.object(subprocess, "Popen", side_effect=interrupted_start):
            with self.assertRaises(SystemExit) as error:
                signing.execute([sys.executable, "-c", "import time; time.sleep(60)"], os.environ)
        self.assertEqual(error.exception.code, 128 + signal.SIGTERM)
        self.assertIsNotNone(children[0].poll())
        self.assertIs(signal.getsignal(signal.SIGTERM), original)

    def test_child_signal_exit_status_is_preserved(self):
        code = "import os, signal; os.kill(os.getpid(), signal.SIGTERM)"
        self.assertEqual(signing.execute([sys.executable, "-c", code], os.environ),
                         128 + signal.SIGTERM)

    def test_parsed_output_excludes_stderr_warnings(self):
        result = subprocess.CompletedProcess([], 0, "tag\n", "warning: test\n")
        with patch.object(subprocess, "run", return_value=result):
            self.assertEqual(signing.run(["git", "cat-file", "-t", "refs/tags/v1.0.0"]), "tag\n")

    def test_failed_commands_identify_operations_without_unsafe_values(self):
        sentinel = "DO-NOT-DISCLOSE"
        result = subprocess.CompletedProcess([], 17, sentinel, sentinel)
        with patch.object(subprocess, "run", return_value=result):
            with self.assertRaisesRegex(signing.SigningError, r"Git verify-commit failed \(exit status 17\)") as error:
                signing.run(["git", "verify-commit", sentinel])
            self.assertNotIn(sentinel, str(error.exception))
            with self.assertRaisesRegex(signing.SigningError, r"GPG key import failed \(exit status 17\)") as error:
                signing.credential_gpg(["--import", sentinel], payload=sentinel,
                                       env={"RELEASE_GPG_PASSPHRASE": sentinel})
            self.assertNotIn(sentinel, str(error.exception))

    def test_command_start_and_timeout_errors_withhold_payloads(self):
        for failure, category in [(OSError("DO-NOT-DISCLOSE"), "OS error"),
                                  (subprocess.TimeoutExpired("DO-NOT-DISCLOSE", 60,
                                                             output="DO-NOT-DISCLOSE"), "timed out")]:
            with self.subTest(category=category), patch.object(subprocess, "run", side_effect=failure):
                with self.assertRaises(signing.SigningError) as error:
                    signing.gpg("--verify", "DO-NOT-DISCLOSE")
                self.assertIn("GPG signature verification", str(error.exception))
                self.assertIn(category, str(error.exception))
                self.assertNotIn("DO-NOT-DISCLOSE", str(error.exception))

    def test_unexpected_failure_identifies_operation_and_safe_category(self):
        for failure, category in [(OSError("DO-NOT-DISCLOSE"), "OS error"),
                                  (RuntimeError("DO-NOT-DISCLOSE"), "internal error")]:
            output = StringIO()
            with patch("sys.argv", ["signing.py", "verify-commit", "HEAD"]), \
                    patch.object(signing, "verify_commit", side_effect=failure), redirect_stderr(output):
                self.assertEqual(signing.main(), 1)
            self.assertIn("verify-commit failed (" + category + ")", output.getvalue())
            self.assertNotIn("DO-NOT-DISCLOSE", output.getvalue())


if __name__ == "__main__":
    unittest.main()
