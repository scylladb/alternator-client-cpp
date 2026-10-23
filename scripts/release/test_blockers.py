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

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import check_blockers


class BlockerTests(unittest.TestCase):
    def query(self, response, code=0):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            gh = directory / "gh"
            gh.write_text(f'#!{sys.executable}\n'
                          'import json, os, sys\nfrom pathlib import Path\n'
                          'Path(os.environ["ARGUMENT_RECORD"]).write_text(json.dumps(sys.argv[1:]))\n'
                          'print(os.environ["FAKE_RESPONSE"])\n'
                          'print("SYNTHETIC-ERROR-PAYLOAD", file=sys.stderr)\n'
                          'raise SystemExit(int(os.environ["FAKE_EXIT"]))\n')
            gh.chmod(0o700)
            record = directory / "arguments"
            result = subprocess.run([sys.executable, check_blockers.__file__, "--repo",
                                     "scylladb/alternator-client-cpp"],
                                    capture_output=True, text=True, timeout=10,
                                    env={**os.environ, "PATH": temp + os.pathsep + os.environ["PATH"],
                                         "ARGUMENT_RECORD": str(record), "FAKE_RESPONSE": response,
                                         "FAKE_EXIT": str(code)})
            return result, json.loads(record.read_text())

    def test_empty_blockers_allow_publication_and_query_only_open_labelled_issues(self):
        result, arguments = self.query("[]")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(arguments, ["issue", "list", "--repo", "scylladb/alternator-client-cpp",
                                    "--state", "open", "--label", "release-blocker", "--limit", "1",
                                    "--json", "number"])

    def test_open_blocker_stops_publication_and_identifies_the_issue(self):
        result, _ = self.query('[{"number": 123}]')
        self.assertEqual(result.returncode, 1)
        self.assertIn("https://github.com/scylladb/alternator-client-cpp/issues/123", result.stderr)

    def test_api_failure_is_not_treated_as_absence(self):
        result, _ = self.query("[]", code=1)
        self.assertEqual(result.returncode, 2)
        self.assertIn("publication is blocked", result.stderr)
        self.assertNotIn("SYNTHETIC-ERROR-PAYLOAD", result.stderr + result.stdout)

    def test_invalid_or_incomplete_responses_fail_closed(self):
        for response in ("invalid-json", "null", "{}", '[{"number":true}]', '[{"number":0}]',
                         '[{"number":"SYNTHETIC-SECRET"}]', '[{"number":1},{"number":2}]'):
            with self.subTest(response=response):
                result, _ = self.query(response)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("SYNTHETIC-SECRET", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
