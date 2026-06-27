cmake_minimum_required(VERSION 3.20)

project(alternator-client-cpp
    VERSION 0.1.0
    DESCRIPTION "ScyllaDB Alternator client-side load balancing helper for AWS SDK for C++"
    LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(ALTERNATOR_CLIENT_CPP_BUILD_TESTS "Build unit tests" ON)
option(ALTERNATOR_CLIENT_CPP_ENABLE_AWS "Build AWS SDK for C++ DynamoDB adapter when AWSSDK is available" ON)

find_package(CURL QUIET)
find_package(OpenSSL QUIET)
find_package(Threads REQUIRED)

add_library(alternator_client_cpp
    src/attribute_value.cpp
    src/config.cpp
    src/go_random.cpp
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
target_include_directories(alternator_client_cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_link_libraries(alternator_client_cpp
    PUBLIC
        Threads::Threads)

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
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        find_package(AWSSDK QUIET COMPONENTS dynamodb)
    else()
        set(AWSSDK_FOUND FALSE)
        message(STATUS "ZLIB not found; AWS DynamoDB adapter target will not be built")
    endif()
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
        message(STATUS "AWSSDK not found; AWS DynamoDB adapter target will not be built")
    endif()
endif()

if(ALTERNATOR_CLIENT_CPP_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(alternator_client_cpp_tests
            tests/attribute_value_test.cpp
            tests/http_client_test.cpp
            tests/key_route_affinity_test.cpp
            tests/live_nodes_integration_test.cpp
            tests/live_nodes_test.cpp
            tests/node_health_test.cpp
            tests/query_plan_test.cpp
            tests/routing_scope_test.cpp)
        target_link_libraries(alternator_client_cpp_tests
            PRIVATE
                alternator_client_cpp
                GTest::gtest_main)
        if(TARGET alternator_client_cpp_aws)
            target_sources(alternator_client_cpp_tests
                PRIVATE
                    tests/aws_integration_test.cpp
                    tests/aws_dynamodb_helper_test.cpp)
            target_link_libraries(alternator_client_cpp_tests
                PRIVATE
                    alternator_client_cpp_aws)
        endif()
        include(GoogleTest)
        gtest_discover_tests(alternator_client_cpp_tests)
    else()
        message(STATUS "GTest not found; tests will not be built")
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
