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

if(NOT DEFINED TEST_EXECUTABLE)
    message(FATAL_ERROR "TEST_EXECUTABLE must be set")
endif()

execute_process(
    COMMAND "${TEST_EXECUTABLE}" --gtest_list_tests
    RESULT_VARIABLE test_list_result
    OUTPUT_VARIABLE test_list_output
    ERROR_VARIABLE test_list_error)

if(NOT test_list_result EQUAL 0)
    message(FATAL_ERROR
        "failed to list gtest tests from ${TEST_EXECUTABLE}:\n${test_list_error}")
endif()

if(test_list_output STREQUAL "")
    message(FATAL_ERROR "${TEST_EXECUTABLE} does not contain any gtest tests")
endif()

if(DEFINED REQUIRED_TEST_REGEX AND NOT test_list_output MATCHES "${REQUIRED_TEST_REGEX}")
    message(FATAL_ERROR
        "${TEST_EXECUTABLE} does not contain a test matching ${REQUIRED_TEST_REGEX}")
endif()
