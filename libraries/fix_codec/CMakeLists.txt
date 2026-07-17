# fix_codec is a header-only, application-tier FIX codec library. It sits above
# pubsub_itc_fw (never inside it) and is the FIX counterpart to scram_crypto:
# a zero-copy reader/writer plus a generated dictionary header. Consumers include
# <fix_codec/...> for the hand-written headers and <fix_codec/fix_dictionary.hpp>
# for the generated one (produced into ${FIX_DICT_INCLUDE_DIR} at build time).
add_library(fix_codec INTERFACE)

target_include_directories(fix_codec INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${FIX_DICT_INCLUDE_DIR}
)

# ── Unit tests ────────────────────────────────────────────────────────────────
add_executable(fix_codec_tests
        tests/FixChecksumTest.cpp
        tests/FixFieldTest.cpp
        tests/FixMessageReaderTest.cpp
        tests/FixMessageWriterTest.cpp
)

target_link_libraries(fix_codec_tests PRIVATE
        fix_codec
        GTest::gtest_main
)

# The generated dictionary header must exist before the tests compile.
add_dependencies(fix_codec_tests fix_dictionary_generated)

include(GoogleTest)
if(ENABLE_TSAN)
    gtest_discover_tests(fix_codec_tests DISCOVERY_MODE PRE_TEST)
else()
    gtest_discover_tests(fix_codec_tests)
endif()

install(TARGETS fix_codec_tests
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
