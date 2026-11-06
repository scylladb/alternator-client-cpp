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

"""Run validation in an unsigned release worktree without production credentials."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import lifecycle

TOOLS = Path(__file__).resolve().parent


def validate(plan_path, skip_tests=False):
    plan_path = Path(plan_path).resolve()
    plan = lifecycle.read_plan(plan_path)
    # Reject credentials rather than exposing them to selected source/build files.
    lifecycle.require(not any(os.environ.get(name) for name in (
        "RELEASE_GPG_PRIVATE_KEY", "RELEASE_GPG_PRIVATE_KEY_FILE", "RELEASE_GPG_PASSPHRASE",
        "RELEASE_GPG_PASSPHRASE_FILE", "GPG_PRIVATE_KEY", "GPG_PASSPHRASE")),
        "Validation must run without signing credentials")
    subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(TOOLS), "-p", "test_*.py", "-v"], check=True)
    with tempfile.TemporaryDirectory(prefix="release-validation-") as temp:
        source = Path(temp) / "source"
        lifecycle.git("worktree", "add", "--detach", str(source), plan["baseline"])
        def guarded(*args):
            subprocess.run([sys.executable, str(TOOLS / "guard.py"), "run", *args], cwd=source, check=True)
        try:
            subprocess.run([sys.executable, str(TOOLS / "lifecycle.py"), "materialize", "--plan", str(plan_path)],
                           cwd=source, check=True)
            guarded("make", "build", "BUILD_CMAKE_FLAGS=-DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=ON")
            guarded("make", "check")
            if not skip_tests:
                guarded("make", "test-unit", "BUILD_CMAKE_FLAGS=-DALTERNATOR_CLIENT_CPP_REQUIRE_AWS=ON")
                guarded("make", "test-integration")
            guarded(sys.executable, str(TOOLS / "package_check.py"), "--plan", str(plan_path))
            # Reject source/index changes made by checks before recording success.
            guarded("git", "diff", "--exit-code", plan["tree"], "--")
            guarded("git", "diff", "--cached", "--exit-code", plan["tree"], "--")
        finally:
            if not skip_tests:
                # Cleanup is best effort and cannot replace a validation failure.
                subprocess.run([sys.executable, str(TOOLS / "guard.py"), "run", "make", "scylla-stop"], cwd=source)
            lifecycle.git("worktree", "remove", "--force", str(source))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", required=True)
    parser.add_argument("--skip-tests", choices=("true", "false"), default="false")
    args = parser.parse_args()
    validate(args.plan, args.skip_tests == "true")
