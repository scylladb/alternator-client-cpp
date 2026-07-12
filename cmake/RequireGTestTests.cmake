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
