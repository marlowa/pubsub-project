# fix_common is header-only: each gateway includes its headers directly. This file builds only its
# unit tests. They are a program of their own because one of them replaces the global operator new
# to count allocations, and that replacement would apply to every test linked with it.
add_executable(fix_common_tests
        tests/PacedResendTest.cpp
        tests/RollingWindowThrottleTest.cpp
        tests/SessionThrottlesTest.cpp
        tests/ThrottleHeapUseTest.cpp
        tests/UnansweredCommandStoreTest.cpp
)

target_include_directories(fix_common_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(fix_common_tests PRIVATE
        pubsub_itc_fw
        tsl::robin_map
        GTest::gtest_main
)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(fix_common_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(fix_common_tests)
endif()

install(TARGETS fix_common_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
