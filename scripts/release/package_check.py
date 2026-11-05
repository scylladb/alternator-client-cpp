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

"""Extract each source format, build/install it, and link core and AWS consumers."""

import argparse
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import zipfile

import lifecycle


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def check(ref, value):
    with tempfile.TemporaryDirectory(prefix="release-package-check-") as temp:
        root = Path(temp)
        archives = lifecycle.package(ref, value, root)
        for number, archive in enumerate(archives):
            extracted = root / f"extract-{number}"
            extracted.mkdir()
            if archive.suffix == ".zip":
                with zipfile.ZipFile(archive) as stream:
                    stream.extractall(extracted)
            else:
                with tarfile.open(archive) as stream:
                    stream.extractall(extracted, filter="data")
            source = extracted / ("alternator-client-cpp-" + value)
            for aws in (False, True):
                variant = root / f"variant-{number}-{aws}"
                build, install = variant / "build", variant / "install"
                run("cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
                    "-DALTERNATOR_CLIENT_CPP_BUILD_TESTS=OFF",
                    "-DALTERNATOR_CLIENT_CPP_ENABLE_AWS=" + ("ON" if aws else "OFF"),
                    "-DCMAKE_INSTALL_PREFIX=" + str(install))
                run("cmake", "--build", build, "--parallel", "2")
                run("cmake", "--install", build)
                consumer = variant / "consumer"
                consumer.mkdir()
                target = "alternator_client_cpp_aws" if aws else "alternator_client_cpp"
                (consumer / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.20)\nproject(Consumer LANGUAGES CXX)\n"
                    f"find_package(scylladb-alternator-client-cpp {value} EXACT CONFIG REQUIRED)\n"
                    "add_executable(consumer main.cpp)\n"
                    f"target_link_libraries(consumer PRIVATE ScyllaDB::{target})\n")
                (consumer / "main.cpp").write_text(
                    "#include <scylladb/alternator/config.h>\n"
                    "#include <string_view>\n"
                    + ("#include <aws/core/Aws.h>\n#include <scylladb/alternator/aws/dynamodb_helper.h>\n" if aws else "")
                    + f'static_assert(std::string_view(SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION) == "{value}");\n'
                    + "int main() { scylladb::alternator::Config config; scylladb::alternator::ValidateConfig(config); "
                    + ("Aws::SDKOptions options; Aws::InitAPI(options); { "
                       "scylladb::alternator::aws::DynamoDBHelper helper({\"localhost\"}, config); } "
                       "Aws::ShutdownAPI(options); " if aws else "")
                    + "return config.port == 0; }\n")
                prefix = str(install) + ";" + os.environ.get("CMAKE_PREFIX_PATH", "")
                run("cmake", "-S", consumer, "-B", variant / "consumer-build", "-DCMAKE_PREFIX_PATH=" + prefix)
                run("cmake", "--build", variant / "consumer-build", "--parallel", "2")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--plan")
    source.add_argument("--ref", help="Build a stable VERSION tree from this commit for PR CI")
    args = parser.parse_args()
    if args.plan:
        plan = lifecycle.read_plan(args.plan)
        check(plan["tree"], plan["version"])
    else:
        baseline = lifecycle.oid(args.ref + "^{commit}")
        value = lifecycle.git("show", baseline + ":VERSION").decode().strip().removesuffix("-SNAPSHOT")
        lifecycle.version(value)
        check(lifecycle.version_tree(baseline, value), value)
