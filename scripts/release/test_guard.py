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

from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import guard
import signing

GUARD = Path(guard.__file__).resolve()


@unittest.skipUnless(sys.platform.startswith("linux"), "Linux process ownership required")
class GuardTests(unittest.TestCase):
    def environment(self, directory):
        return {"PATH": os.environ["PATH"], "PYTHONDONTWRITEBYTECODE": "1",
                "RELEASE_GPG_PASSPHRASE": "synthetic %secret+/with'quotes$",
                "RELEASE_LOG_DIR": str(directory / "logs")}

    def test_redacts_complete_streams_and_encoded_forms_in_saved_logs(self):
        code = '''import base64, json, os, sys
from urllib.parse import quote, quote_plus
s = os.environ["RELEASE_GPG_PASSPHRASE"]
forms = [s, base64.b64encode(s.encode()).decode(), s.encode().hex(),
         quote(s, safe=""), quote_plus(s, safe=""), json.dumps(s)[1:-1]]
for value in forms:
    raw = value.encode()
    os.write(1, raw[:3])
    os.write(1, raw[3:] + b"\\n")
    print(value, file=sys.stderr)
'''
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            result = subprocess.run([sys.executable, GUARD, "run", sys.executable, "-c", code],
                                    env=self.environment(directory), capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            for name, stream in (("stdout", result.stdout), ("stderr", result.stderr)):
                self.assertEqual(stream, b"[REDACTED]\n" * 6)
                self.assertEqual((directory / "logs" / (name + ".log")).read_bytes(), stream)

    def test_passphrase_file_and_private_export_inputs_are_redacted(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            env = self.environment(directory)
            secret_file = directory / "passphrase"
            secret_file.write_text(env.pop("RELEASE_GPG_PASSPHRASE") + "\n")
            env["RELEASE_GPG_PASSPHRASE_FILE"] = str(secret_file)
            env["RELEASE_GPG_PRIVATE_KEY"] = "SYNTHETIC-PRIVATE-EXPORT"
            code = ('import os; from pathlib import Path; '
                    'print(Path(os.environ["RELEASE_GPG_PASSPHRASE_FILE"]).read_text().strip()); '
                    'print(os.environ["RELEASE_GPG_PASSPHRASE_FILE"]); '
                    'print(os.environ["RELEASE_GPG_PRIVATE_KEY"])')
            result = subprocess.run([sys.executable, GUARD, "run", sys.executable, "-c", code],
                                    env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, b"[REDACTED]\n" * 3)

    def test_rejects_credentials_and_trace_modes_before_starting(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self.environment(Path(temp))
            for command in (["echo", env["RELEASE_GPG_PASSPHRASE"]],
                            ["echo", "--password=not-a-real-password"],
                            ["sh", "-exc", "echo unsafe"], ["echo", "--debug"],
                            ["make", "--debug=a"], ["make", "-d"],
                            ["bash", "-o", "xtrace", "-c", "echo harmless"],
                            ["bash", "-e", "-x", "-c", "echo harmless"]):
                with self.subTest(command=command), patch.object(subprocess, "Popen") as launch:
                    with self.assertRaises(signing.SigningError):
                        signing.execute(command, env)
                    launch.assert_not_called()
            for flags in ("d", "wd", "--debug=a", "-d"):
                with self.subTest(flags=flags), patch.object(subprocess, "Popen") as launch:
                    with self.assertRaises(signing.SigningError):
                        signing.execute(["make", "build"], {**env, "MAKEFLAGS": flags})
                    launch.assert_not_called()
            guard.check_command(["make", "build"], env, guard.secret_forms(env))
            with patch.object(signing, "signing_session") as session, redirect_stderr(StringIO()), \
                    patch("sys.argv", ["signing.py", "exec", "make", "--debug=a"]):
                self.assertEqual(signing.main(), 1)
                session.assert_not_called()

    def test_failed_command_preserves_status_and_sanitizes_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            env = self.environment(Path(temp))
            code = 'import os; print(os.environ["RELEASE_GPG_PASSPHRASE"]); raise SystemExit(7)'
            result = subprocess.run([sys.executable, GUARD, "run", sys.executable, "-c", code],
                                    env=env, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 7, result.stderr)
            self.assertEqual(result.stdout, b"[REDACTED]\n")

    def test_overflow_withholds_all_raw_output_and_stops_command(self):
        output, errors = StringIO(), StringIO()
        with patch.object(guard, "MAX_OUTPUT", 64), redirect_stdout(output), redirect_stderr(errors):
            with self.assertRaises(signing.SigningError):
                signing.execute([sys.executable, "-c", 'print("sensitive-output" * 10000)'], {})
        self.assertNotIn("sensitive-output", output.getvalue() + errors.getvalue())

    def test_log_write_failure_does_not_replace_failed_command_status(self):
        with tempfile.TemporaryDirectory() as temp:
            location = Path(temp) / "not-a-directory"
            location.write_text("unchanged")
            for status in (0, 7):
                with self.subTest(status=status), redirect_stdout(StringIO()), redirect_stderr(StringIO()):
                    command = [sys.executable, "-c", f"raise SystemExit({status})"]
                    if status:
                        self.assertEqual(signing.execute(command, {"RELEASE_LOG_DIR": str(location)}), 7)
                    else:
                        with self.assertRaises(signing.SigningError):
                            signing.execute(command, {"RELEASE_LOG_DIR": str(location)})
            self.assertEqual(location.read_text(), "unchanged")

    def test_overflow_preserves_an_existing_command_failure(self):
        real_stop = guard.ProcessTree.stop
        def observe_failure(owner, process, signum):
            # Force the reviewed schedule: output was read before poll observed
            # the command's own nonzero exit, without terminating that command.
            process.wait(timeout=5)
            return real_stop(owner, process, signum)
        with patch.object(guard, "MAX_OUTPUT", 64), \
                patch.object(guard.ProcessTree, "stop", observe_failure), \
                redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            self.assertEqual(signing.execute([sys.executable, "-c",
                'import os; os.write(1, b"x" * 100); raise SystemExit(7)'], {}), 7)

    def test_descendant_cleanup_error_preserves_failed_status(self):
        real_stop = guard.ProcessTree.stop
        def failed_cleanup(owner, process, signum):
            real_stop(owner, process, signum)
            raise RuntimeError("SYNTHETIC-SECRET-IN-EXCEPTION")
        for status in (0, 7):
            output, errors = StringIO(), StringIO()
            with self.subTest(status=status), patch.object(guard.ProcessTree, "stop", failed_cleanup), \
                    redirect_stdout(output), redirect_stderr(errors):
                command = [sys.executable, "-c", f"raise SystemExit({status})"]
                if status:
                    self.assertEqual(signing.execute(command, {}), status)
                else:
                    with self.assertRaises(signing.SigningError):
                        signing.execute(command, {})
            self.assertNotIn("SYNTHETIC-SECRET", output.getvalue() + errors.getvalue())

    def test_capture_read_error_preserves_failed_status(self):
        real_popen, real_read = subprocess.Popen, guard.Capture.read
        for status in (0, 7):
            with self.subTest(status=status), tempfile.TemporaryDirectory() as temp:
                release = Path(temp) / "release-child"
                children = []
                def launch(*args, **kwargs):
                    child = real_popen(*args, **kwargs)
                    children.append(child)
                    return child
                def fail_read(capture, *args, **kwargs):
                    if not release.exists():
                        release.touch()
                        children[0].wait(timeout=5)
                        raise OSError("SYNTHETIC-SECRET-IN-EXCEPTION")
                    return real_read(capture, *args, **kwargs)
                command = [sys.executable, "-c",
                           'import time; from pathlib import Path; '
                           f'p = Path({str(release)!r}); '
                           '\nwhile not p.exists(): time.sleep(0.01)\n'
                           f'raise SystemExit({status})']
                output, errors = StringIO(), StringIO()
                with patch.object(subprocess, "Popen", launch), \
                        patch.object(guard.Capture, "read", fail_read), \
                        redirect_stdout(output), redirect_stderr(errors):
                    if status:
                        self.assertEqual(signing.execute(command, {}), status)
                    else:
                        with self.assertRaises(signing.SigningError):
                            signing.execute(command, {})
                self.assertNotIn("SYNTHETIC-SECRET", output.getvalue() + errors.getvalue())

    def test_preexisting_children_are_not_owned(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
        try:
            self.assertEqual(signing.execute([sys.executable, "-c", "pass"], {}), 0)
            self.assertIsNone(unrelated.poll())
        finally:
            unrelated.kill()
            unrelated.wait()

    def test_detached_descendants_are_stopped_on_success_and_cancellation(self):
        for cancel in (False, True):
            with self.subTest(cancel=cancel), tempfile.TemporaryDirectory() as temp:
                directory = Path(temp)
                state = directory / "child.json"
                child_code = ('import json, os, signal, time; from pathlib import Path; '
                              'signal.signal(signal.SIGTERM, signal.SIG_IGN); '
                              'p = Path(os.environ["CHILD_STATE"]); '
                              'q = p.with_suffix(".tmp"); '
                              'q.write_text(json.dumps({"pid": os.getpid()})); q.replace(p); '
                              'time.sleep(60)')
                parent_code = ('import os, subprocess, sys, time; from pathlib import Path; '
                               f'subprocess.Popen([sys.executable, "-c", {child_code!r}], '
                               'start_new_session=True); '
                               'p = Path(os.environ["CHILD_STATE"]); '
                               '\nwhile not p.exists(): time.sleep(0.01)\n' +
                               ('time.sleep(60)' if cancel else 'raise SystemExit(0)'))
                process = subprocess.Popen(
                    [sys.executable, GUARD, "run", sys.executable, "-c", parent_code],
                    env={**self.environment(directory), "CHILD_STATE": str(state)},
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                pid = None
                try:
                    deadline = time.monotonic() + 10
                    while not state.exists() and process.poll() is None and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertTrue(state.exists(), "Detached child did not start")
                    pid = json.loads(state.read_text())["pid"]
                    if cancel:
                        process.send_signal(signal.SIGTERM)
                    _, errors = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, 143 if cancel else 0, errors)
                    self.assertFalse(Path(f"/proc/{pid}").exists(), "Detached child was not reaped")
                finally:
                    if process.poll() is None:
                        process.kill()
                    if pid is not None:
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
