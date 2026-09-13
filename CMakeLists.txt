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

cmake_minimum_required(VERSION 3.20)

project(alternator-client-cpp
    VERSION 0.1.0
    DESCRIPTION "ScyllaDB Alternator client-side load balancing helper for AWS SDK for C++"
    LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(ALTERNATOR_CLIENT_CPP_BUILD_TESTS "Build unit tests" ON)
option(ALTERNATOR_CLIENT_CPP_ENABLE_AWS "Build AWS SDK for C++ DynamoDB adapter when AWSSDK is available" ON)
option(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS "Fail configuration unless the AWS SDK adapter and its tests are built" OFF)

if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS AND NOT ALTERNATOR_CLIENT_CPP_ENABLE_AWS)
    message(FATAL_ERROR "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS requires ALTERNATOR_CLIENT_CPP_ENABLE_AWS=ON")
endif()

if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS AND NOT ALTERNATOR_CLIENT_CPP_BUILD_TESTS)
    message(FATAL_ERROR "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS requires ALTERNATOR_CLIENT_CPP_BUILD_TESTS=ON")
endif()

find_package(CURL QUIET)
find_package(OpenSSL QUIET)
find_package(Threads REQUIRED)
find_package(ZLIB QUIET)
find_package(yaml-cpp QUIET)

set(ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET "")
if(TARGET yaml-cpp::yaml-cpp)
    set(ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET yaml-cpp)
endif()

add_library(alternator_client_cpp
    src/attribute_value.cpp
    src/config.cpp
    src/go_random.cpp
    src/http_compression.cpp
    src/http_client.cpp
    src/key_route_affinity.cpp
    src/live_nodes.cpp
    src/murmur3.cpp
    src/node_health.cpp
    src/query_plan.cpp
    src/routing_scope.cpp
    src/uri.cpp)

add_library(ScyllaDB::alternator_client_cpp ALIAS alternator_client_cpp)

target_compile_features(alternator_client_cpp PUBLIC cxx_std_17)
target_compile_definitions(alternator_client_cpp
    PUBLIC
        SCYLLADB_ALTERNATOR_CLIENT_CPP_VERSION="${PROJECT_VERSION}")
target_include_directories(alternator_client_cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(alternator_client_cpp
    PUBLIC
        Threads::Threads)

if(ZLIB_FOUND)
    target_link_libraries(alternator_client_cpp PUBLIC ZLIB::ZLIB)
    target_compile_definitions(alternator_client_cpp PUBLIC SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_ZLIB=1)
else()
    message(STATUS "ZLIB not found; gzip request encoding and gzip/deflate response decoding are unavailable")
endif()

if(CURL_FOUND)
    target_link_libraries(alternator_client_cpp PUBLIC CURL::libcurl)
    target_compile_definitions(alternator_client_cpp PUBLIC SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_CURL=1)
    if(OpenSSL_FOUND)
        target_link_libraries(alternator_client_cpp PUBLIC OpenSSL::SSL)
        target_compile_definitions(alternator_client_cpp PUBLIC SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_OPENSSL=1)
    endif()
else()
    message(STATUS "libcurl development package not found; default discovery client supports plain HTTP only")
endif()

if(ALTERNATOR_CLIENT_CPP_ENABLE_AWS)
    find_package(AWSSDK QUIET COMPONENTS dynamodb)
    if(AWSSDK_FOUND)
        add_library(alternator_client_cpp_aws
            src/aws_dynamodb_helper.cpp)
        add_library(ScyllaDB::alternator_client_cpp_aws ALIAS alternator_client_cpp_aws)
        target_compile_features(alternator_client_cpp_aws PUBLIC cxx_std_17)
        target_include_directories(alternator_client_cpp_aws
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:include>)
        target_link_libraries(alternator_client_cpp_aws
            PUBLIC
                alternator_client_cpp
                ${AWSSDK_LINK_LIBRARIES})
        target_compile_definitions(alternator_client_cpp_aws
            PUBLIC SCYLLADB_ALTERNATOR_CLIENT_CPP_HAS_AWS_SDK=1)
    else()
        if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS)
            message(FATAL_ERROR "AWSSDK with the dynamodb component not found; AWS DynamoDB adapter target cannot be built")
        else()
            message(STATUS "AWSSDK not found; AWS DynamoDB adapter target will not be built")
        endif()
    endif()
endif()

if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS AND NOT TARGET alternator_client_cpp_aws)
    message(FATAL_ERROR "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS requires the alternator_client_cpp_aws target")
endif()

if(ALTERNATOR_CLIENT_CPP_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_CROSSCOMPILING)
            include(CheckCXXSourceRuns)
            check_cxx_source_runs([=[
                #include <sys/syscall.h>
                #include <unistd.h>

                int main() {
                #if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
                    const int pid_fd = static_cast<int>(::syscall(SYS_pidfd_open, ::getpid(), 0));
                    if (pid_fd < 0) {
                        return 1;
                    }
                    const long result = ::syscall(
                        SYS_pidfd_send_signal, pid_fd, 0, nullptr, 0);
                    ::close(pid_fd);
                    return result == 0 ? 0 : 1;
                #else
                    return 1;
                #endif
                }
            ]=] ALTERNATOR_CLIENT_CPP_WORKING_PIDFD)
        else()
            set(ALTERNATOR_CLIENT_CPP_WORKING_PIDFD OFF)
        endif()

        set(ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES
            tests/attribute_value_test.cpp
            tests/config_test.cpp
            tests/header_optimization_test.cpp
            tests/http_client_test.cpp
            tests/key_route_affinity_test.cpp
            tests/live_nodes_test.cpp
            tests/node_health_test.cpp
            tests/query_plan_test.cpp
            tests/routing_scope_test.cpp)

        if(ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET)
            list(APPEND ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES
                tests/cluster_spec_test.cpp
                tests/testinfra/cluster_spec.cpp)
        else()
            list(REMOVE_ITEM ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES
                tests/cluster_spec_test.cpp)
            message(STATUS "Cluster-spec tests require yaml-cpp")
        endif()

        set(ALTERNATOR_CLIENT_CPP_CCM_TESTINFRA_AVAILABLE OFF)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
                AND CURL_FOUND
                AND OpenSSL_FOUND
                AND ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET
                AND ALTERNATOR_CLIENT_CPP_WORKING_PIDFD)
            add_executable(alternator_client_cpp_ccm_fake_command
                tests/ccm_fake_command.cpp)
            target_compile_features(alternator_client_cpp_ccm_fake_command PRIVATE cxx_std_17)

            add_library(alternator_client_cpp_testinfra
                tests/testinfra/ccm_provisioner.cpp
                tests/testinfra/ccm_run_state.cpp
                tests/testinfra/cluster_spec.cpp
                tests/testinfra/dynamodb_test_client.cpp
                tests/testinfra/test_cluster.cpp)
            target_compile_features(alternator_client_cpp_testinfra PUBLIC cxx_std_17)
            target_include_directories(alternator_client_cpp_testinfra
                PUBLIC
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/testinfra)
            target_link_libraries(alternator_client_cpp_testinfra
                PUBLIC
                    alternator_client_cpp
                    CURL::libcurl
                    OpenSSL::Crypto
                    OpenSSL::SSL
                    ${ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET})
            set(ALTERNATOR_CLIENT_CPP_CCM_TESTINFRA_AVAILABLE ON)
            list(APPEND ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES
                tests/ccm_provisioner_test.cpp
                tests/ccm_run_state_test.cpp
                tests/test_cluster_pool_test.cpp)
            list(REMOVE_ITEM ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES
                tests/testinfra/cluster_spec.cpp)
        else()
            message(STATUS
                "Native CCM test infrastructure requires Linux, libcurl, OpenSSL, yaml-cpp, and working pidfd syscalls")
        endif()

        add_executable(alternator_client_cpp_tests
            ${ALTERNATOR_CLIENT_CPP_CORE_TEST_SOURCES})
        if(ALTERNATOR_CLIENT_CPP_CCM_TESTINFRA_AVAILABLE)
            add_dependencies(
                alternator_client_cpp_tests
                alternator_client_cpp_ccm_fake_command)
        endif()
        target_link_libraries(alternator_client_cpp_tests
            PRIVATE
                alternator_client_cpp
                GTest::gtest_main)
        if(ALTERNATOR_CLIENT_CPP_CCM_TESTINFRA_AVAILABLE)
            target_link_libraries(alternator_client_cpp_tests
                PRIVATE alternator_client_cpp_testinfra)
        elseif(ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET)
            target_link_libraries(alternator_client_cpp_tests
                PRIVATE ${ALTERNATOR_CLIENT_CPP_YAML_CPP_TARGET})
        endif()
        gtest_discover_tests(alternator_client_cpp_tests)

        set(ALTERNATOR_CLIENT_CPP_AWS_TEST_SOURCES
            tests/aws_dynamodb_helper_test.cpp)

        if(TARGET alternator_client_cpp_aws)
            if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS AND NOT ALTERNATOR_CLIENT_CPP_AWS_TEST_SOURCES)
                message(FATAL_ERROR "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS requires AWS adapter test sources")
            endif()

            add_executable(alternator_client_cpp_aws_tests
                ${ALTERNATOR_CLIENT_CPP_AWS_TEST_SOURCES})
            target_link_libraries(alternator_client_cpp_aws_tests
                PRIVATE
                    alternator_client_cpp_aws
                    GTest::gtest_main)
            gtest_discover_tests(alternator_client_cpp_aws_tests)

            add_test(
                NAME alternator_client_cpp_aws_tests_present
                COMMAND ${CMAKE_COMMAND}
                    -DTEST_EXECUTABLE=$<TARGET_FILE:alternator_client_cpp_aws_tests>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RequireGTestTests.cmake)
            if(ALTERNATOR_CLIENT_CPP_CCM_TESTINFRA_AVAILABLE)
                add_executable(alternator_client_cpp_integration_tests
                    tests/aws_integration_test.cpp
                    tests/integration_test_config.cpp
                    tests/integration_test_main.cpp
                    tests/live_nodes_integration_test.cpp)
                target_link_libraries(alternator_client_cpp_integration_tests
                    PRIVATE
                        alternator_client_cpp_aws
                        alternator_client_cpp_testinfra
                        GTest::gtest)
                add_test(
                    NAME alternator_client_cpp_integration_suite
                    COMMAND alternator_client_cpp_integration_tests)
                set_tests_properties(alternator_client_cpp_integration_suite
                    PROPERTIES
                        LABELS Integration
                        TIMEOUT 1200)

                add_executable(alternator_client_cpp_ccm_provisioning_tests
                    tests/cluster_provisioning_integration_test.cpp
                    tests/integration_test_config.cpp
                    tests/integration_test_main.cpp)
                target_link_libraries(alternator_client_cpp_ccm_provisioning_tests
                    PRIVATE
                        alternator_client_cpp_aws
                        alternator_client_cpp_testinfra
                        GTest::gtest)
                add_test(
                    NAME alternator_client_cpp_ccm_provisioning_integration
                    COMMAND alternator_client_cpp_ccm_provisioning_tests)
                set_tests_properties(alternator_client_cpp_ccm_provisioning_integration
                    PROPERTIES
                        LABELS CcmProvisioning
                        TIMEOUT 1200)
                add_test(
                    NAME alternator_client_cpp_aws_integration_tests_present
                    COMMAND ${CMAKE_COMMAND}
                        -DTEST_EXECUTABLE=$<TARGET_FILE:alternator_client_cpp_integration_tests>
                        -DREQUIRED_TEST_REGEX=AwsDynamoDBIntegration\\.
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RequireGTestTests.cmake)
            elseif(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS)
                message(FATAL_ERROR
                    "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS integration tests require native CCM test infrastructure")
            endif()
        endif()

        if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS AND NOT TARGET alternator_client_cpp_aws_tests)
            message(FATAL_ERROR "ALTERNATOR_CLIENT_CPP_REQUIRE_AWS requires the alternator_client_cpp_aws_tests target")
        endif()
    else()
        if(ALTERNATOR_CLIENT_CPP_REQUIRE_AWS)
            message(FATAL_ERROR "GTest not found; AWS adapter tests cannot be built")
        else()
            message(STATUS "GTest not found; tests will not be built")
        endif()
    endif()
endif()

install(TARGETS alternator_client_cpp
    EXPORT alternator_client_cpp_targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(TARGET alternator_client_cpp_aws)
    install(TARGETS alternator_client_cpp_aws
        EXPORT alternator_client_cpp_targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

if(TARGET alternator_client_cpp_aws)
    set(ALTERNATOR_CLIENT_CPP_AWS_TARGET_BUILT TRUE)
else()
    set(ALTERNATOR_CLIENT_CPP_AWS_TARGET_BUILT FALSE)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/scylladb-alternator-client-cpp-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

configure_package_config_file(
    cmake/scylladb-alternator-client-cpp-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/scylladb-alternator-client-cpp-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scylladb-alternator-client-cpp)

install(EXPORT alternator_client_cpp_targets
    NAMESPACE ScyllaDB::
    FILE scylladb-alternator-client-cpp-targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scylladb-alternator-client-cpp)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/scylladb-alternator-client-cpp-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/scylladb-alternator-client-cpp-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/scylladb-alternator-client-cpp)

install(FILES THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
