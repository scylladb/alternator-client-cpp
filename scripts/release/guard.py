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

"""Bounded, credential-safe command logs and Linux descendant ownership."""

import base64
import ctypes
import html
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import signal
import subprocess
import sys
import time
from urllib.parse import quote, quote_plus

MAX_OUTPUT = 8 * 1024 * 1024
GRACE = 2


class GuardError(ValueError):
    pass


def secret_forms(env):
    values = []
    for name, value in env.items():
        if name.endswith(("_TOKEN", "_PASSWORD", "_PASSPHRASE", "_PRIVATE_KEY",
                          "_SECRET_ACCESS_KEY")) and value:
            values.append(value)
    for name in ("RELEASE_GPG_PRIVATE_KEY_FILE", "RELEASE_GPG_PASSPHRASE_FILE"):
        if env.get(name):
            values.append(env[name])
            with open(env[name], "rb") as source:
                value = source.read(1024 * 1024 + 1)
            if len(value) > 1024 * 1024:
                raise GuardError("Credential input exceeds the supported size; values withheld.")
            values.append(value.decode("utf-8").removesuffix("\n"))
    if env.get("GNUPGHOME"):
        values.append(env["GNUPGHOME"])
    for index in range(int(env.get("GIT_CONFIG_COUNT", "0"))):
        if env.get(f"GIT_CONFIG_KEY_{index}") == "gpg.program":
            values.append(env[f"GIT_CONFIG_VALUE_{index}"])
    forms = set()
    for value in values:
        if not value:
            continue
        raw = value.encode()
        forms.update((raw, base64.b64encode(raw), base64.urlsafe_b64encode(raw),
                      raw.hex().encode(), raw.hex().upper().encode(),
                      quote(value, safe="").encode(), quote_plus(value, safe="").encode(),
                      json.dumps(value)[1:-1].encode(),
                      json.dumps(value, ensure_ascii=False)[1:-1].encode(),
                      shlex.quote(value).encode(), html.escape(value).encode()))
        if "-----BEGIN PGP PRIVATE KEY BLOCK-----" in value:
            lines = [line for line in value.splitlines()
                     if len(line) >= 8 and not line.startswith("-----")]
            forms.update(line.encode() for line in lines)
            forms.add("".join(lines).encode())
    forms.update(value.rstrip(b"=") for value in tuple(forms) if value)
    return sorted((value for value in forms if value), key=len, reverse=True)


def check_command(command, env, secrets):
    if not command:
        raise GuardError("Expected a command.")
    for argument in command:
        if (any(value in os.fsencode(argument) for value in secrets)
                or re.search(r"(?i)(password|passphrase|token|secret|private[-_]key)=", argument)):
            raise GuardError("Credentials in command arguments are forbidden; values withheld.")
    inherited = shlex.split(env.get("MAKEFLAGS", ""))
    make_flags = list(inherited)
    if Path(command[0]).name in ("make", "gmake"):
        make_flags += command[1:]
    debug_make = (any(arg.startswith("--debug") or (
        arg.startswith("-") and not arg.startswith("--") and "d" in arg)
        for arg in make_flags) or bool(inherited and inherited[0].isalpha() and "d" in inherited[0]))
    if (any(arg in ("--debug", "--trace", "-x") or arg.startswith("--debug=")
            for arg in command[1:]) or debug_make
            or (Path(command[0]).name in ("sh", "bash", "dash", "zsh")
                and (any(arg in ("xtrace", "verbose", "--verbose") for arg in command[1:])
                     or any(arg.startswith("-") and not arg.startswith("--")
                            and any(flag in arg for flag in "xv") for arg in command[1:])))
            or "xtrace" in env.get("SHELLOPTS", "").split(":")
            or any(env.get(name, "") not in ("", "0", "false") for name in
                   ("GIT_TRACE", "GIT_TRACE_PACKET", "GIT_TRACE_CURL", "GIT_CURL_VERBOSE"))):
        raise GuardError("Debug or trace output is forbidden for guarded commands.")


class ProcessTree:
    """Adopt double-forked/setsid descendants before they can escape cleanup.

    Enable subreaping only during execution, preserving pre-existing children and
    the caller's prior setting. /proc identities prevent signalling recycled PIDs.
    """

    @staticmethod
    def children(pid):
        try:
            return [int(value) for value in
                    Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]
        except FileNotFoundError:
            return []

    @staticmethod
    def identity(pid):
        try:
            fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
            return fields[19], fields[0], int(fields[1])
        except FileNotFoundError:
            return None

    def __enter__(self):
        if not sys.platform.startswith("linux") or not Path("/proc/self/task").exists():
            raise GuardError("Guarded command execution requires Linux with /proc.")
        self.libc = ctypes.CDLL(None, use_errno=True)
        previous = ctypes.c_int()
        if self.libc.prctl(37, ctypes.byref(previous), 0, 0, 0) != 0:
            raise GuardError("Cannot inspect descendant ownership; command was not started.")
        self.previous = previous.value
        self.baseline = set(self.children(os.getpid()))
        if self.libc.prctl(36, 1, 0, 0, 0) != 0:
            raise GuardError("Cannot establish descendant ownership; command was not started.")
        return self

    def __exit__(self, exception_type, *unused):
        if self.libc.prctl(36, self.previous, 0, 0, 0) != 0:
            if exception_type is not None:
                print("Descendant ownership restoration failed; values withheld.", file=sys.stderr)
                return
            raise GuardError("Cannot restore descendant ownership.")

    def owned(self, root):
        pending = [pid for pid in self.children(os.getpid()) if pid not in self.baseline]
        if root.poll() is None:
            pending.append(root.pid)
        found = {}
        while pending:
            pid = pending.pop()
            if pid in found:
                continue
            identity = self.identity(pid)
            if identity is None:
                continue
            found[pid] = identity
            pending.extend(self.children(pid))
        return found

    def stop(self, root, signum):
        deadline = time.monotonic() + GRACE
        sent = set()
        while True:
            live = {}
            for pid, identity in self.owned(root).items():
                if identity[1] == "Z":
                    if identity[2] == os.getpid() and pid != root.pid:
                        try:
                            os.waitpid(pid, os.WNOHANG)
                        except ChildProcessError:
                            pass
                else:
                    live[pid] = identity
            if not live:
                root.wait()
                return
            force = time.monotonic() >= deadline
            for pid, identity in live.items():
                current = self.identity(pid)
                if ((force or (pid, identity[0]) not in sent)
                        and current is not None and current[0] == identity[0]):
                    try:
                        os.kill(pid, signal.SIGKILL if force else signum)
                    except ProcessLookupError:
                        pass
                    sent.add((pid, identity[0]))
            if time.monotonic() > deadline + GRACE:
                raise GuardError("Owned command processes did not stop; output withheld.")
            time.sleep(0.02)


class Capture:
    def __init__(self, process):
        self.selector = selectors.DefaultSelector()
        self.buffers = [bytearray(), bytearray()]
        self.total = 0
        self.overflow = False
        for index, stream in enumerate((process.stdout, process.stderr)):
            os.set_blocking(stream.fileno(), False)
            self.selector.register(stream, selectors.EVENT_READ, index)

    def read(self, timeout=0.1):
        for key, _ in self.selector.select(timeout):
            chunk = os.read(key.fd, 65536)
            if not chunk:
                self.selector.unregister(key.fileobj)
                key.fileobj.close()
            else:
                self.total += len(chunk)
                if self.total > MAX_OUTPUT:
                    self.overflow = True
                    self.buffers = [bytearray(), bytearray()]
                if not self.overflow:
                    self.buffers[key.data].extend(chunk)

    def finish(self, secrets, env):
        deadline = time.monotonic() + GRACE
        try:
            while self.selector.get_map() and time.monotonic() < deadline:
                self.read()
            complete = not self.selector.get_map() and not self.overflow
            streams = [bytes(value) for value in self.buffers] if complete else [
                b"", b"Command output withheld: incomplete or over the size limit.\n"]
            for index, value in enumerate(streams):
                for secret in secrets:
                    value = value.replace(secret, b"[REDACTED]")
                streams[index] = value
            # Raw output exists only in bounded memory. Only sanitized bytes can
            # reach either the terminal or persistent diagnostic artifacts.
            if env.get("RELEASE_LOG_DIR"):
                directory = Path(env["RELEASE_LOG_DIR"])
                directory.mkdir(mode=0o700, parents=True, exist_ok=True)
                for name, data in zip(("stdout.log", "stderr.log"), streams):
                    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW
                    with os.fdopen(os.open(directory / name, flags, 0o600), "wb") as output:
                        output.write(data)
            for output, data in zip((sys.stdout, sys.stderr), streams):
                if hasattr(output, "buffer"):
                    output.buffer.write(data)
                else:
                    output.write(data.decode("utf-8", errors="replace"))
                output.flush()
            if not complete:
                raise GuardError("Command output was incomplete or exceeded the size limit.")
        finally:
            for key in list(self.selector.get_map().values()):
                self.selector.unregister(key.fileobj)
                key.fileobj.close()
            self.selector.close()


def execute(command, env, interrupted, sensitive_env=None):
    secrets = secret_forms({**(sensitive_env or {}), **env})
    check_command(command, env, secrets)
    process = capture = None
    status = None
    failure_status = None
    guard_failure = None
    try:
        with ProcessTree() as owner:
            try:
                interrupted.check()
                process = subprocess.Popen(command, env=env, start_new_session=True,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                capture = Capture(process)
                while process.poll() is None:
                    interrupted.check()
                    capture.read()
                    if capture.overflow:
                        raise GuardError("Command output exceeded the size limit; values withheld.")
                interrupted.check()
                status = process.returncode
            except GuardError as error:
                guard_failure = error
            except Exception:
                guard_failure = GuardError("Command startup or output capture failed; values withheld.")
            finally:
                if process is not None:
                    finished = process.poll()
                    if finished is not None and finished != 0:
                        failure_status = finished if finished > 0 else 128 - finished
                    try:
                        owner.stop(process, interrupted.signum or signal.SIGTERM)
                    except Exception:
                        guard_failure = GuardError("Descendant cleanup failed; values withheld.")
                    # A command can finish with its own failure while overflow
                    # triggers cleanup. Preserve that result, but do not mistake
                    # the guard's own termination signal for a build failure.
                    if failure_status is None and process.returncode is not None and process.returncode > 0:
                        failure_status = process.returncode
                if capture is not None:
                    try:
                        capture.finish(secrets, env)
                    except Exception:
                        guard_failure = GuardError("Guarded log capture failed; values withheld.")
                elif process is not None:
                    for stream in (process.stdout, process.stderr):
                        if stream is not None:
                            stream.close()
                if interrupted.signum is not None and guard_failure is not None:
                    print("Guard cleanup failed during cancellation; values withheld.", file=sys.stderr)
    except GuardError as error:
        guard_failure = error
    if guard_failure is not None:
        if failure_status is not None:
            print("Guard cleanup or output capture failed; values withheld.", file=sys.stderr)
            return failure_status
        raise guard_failure
    return status if status >= 0 else 128 - status


def main():
    import signing
    try:
        if len(sys.argv) < 3 or sys.argv[1] != "run":
            raise GuardError("Expected run COMMAND...")
        return signing.execute(sys.argv[2:], os.environ)
    except (GuardError, signing.SigningError):
        print("Guarded command failed; values withheld.", file=sys.stderr)
        return 1
    except Exception:
        print("Guarded command failed (internal error); values withheld.", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
