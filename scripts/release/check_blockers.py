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

"""Fail closed when release-blocker issues are open or cannot be checked."""

import argparse
import json
import re
import subprocess
import sys


def check(repository):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Expected OWNER/REPOSITORY")
    # The REST issues endpoint includes pull requests; slurp preserves page boundaries.
    result = subprocess.run(["gh", "api", "--paginate", "--slurp",
                             f"repos/{repository}/issues?state=open&labels=release-blocker&per_page=100"],
                            capture_output=True, text=True, timeout=60, check=True)
    pages = json.loads(result.stdout)
    if not isinstance(pages, list) or not pages or any(not isinstance(page, list) for page in pages):
        raise ValueError("Invalid paginated issue response")
    issues = [issue for page in pages for issue in page]
    if any(not isinstance(issue, dict) or type(issue.get("number")) is not int
           or issue["number"] <= 0 for issue in issues):
        raise ValueError("Invalid issue response")
    if issues:
        for issue in issues:
            kind = "pull" if "pull_request" in issue else "issues"
            print(f"Release blocked: https://github.com/{repository}/{kind}/{issue['number']}", file=sys.stderr)
        return 1
    print("No open release-blocker issues or PRs.")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    args = parser.parse_args()
    try:
        return check(args.repo)
    except Exception:
        print("Cannot verify release blockers; publication is blocked. Values withheld.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
