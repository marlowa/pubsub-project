# fix_common is header-only: each gateway includes its headers directly. This file builds only its
# unit tests. They are a program of their own because one of them replaces the global operator new
# to count allocations, and that replacement would apply to every test linked with it.
add_executable(fix_common_tests
        tests/RefusedCommandTest.cpp
        tests/RollingWindowThrottleTest.cpp
        tests/SessionThrottlesTest.cpp
        tests/ThrottleHeapUseTest.cpp
        tests/VoidedCommandListTest.cpp
)

# ThrottleHeapUseTest.cpp replaces the global operator new and delete to count heap use, and declares them without
# noexcept, which the project's rules do not allow. A deallocation function is noexcept whether or not it says so, so
# the replacement means the same either way; Clang nevertheless reports that it differs from the declaration in <new>,
# and GCC does not.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_source_files_properties(tests/ThrottleHeapUseTest.cpp PROPERTIES COMPILE_OPTIONS "-Wno-implicit-exception-spec-mismatch")
endif()

target_include_directories(fix_common_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${FIX_DSL_INCLUDE_DIR}
)

# VoidedCommandList.hpp uses the application's copy of the messages in leader_follower.dsl.
add_dependencies(fix_common_tests leader_follower_generated fix_orders_generated)

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
