add_library(scram_crypto STATIC ScramCrypto.cpp)

target_include_directories(scram_crypto PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# ScramCrypto.hpp exposes no OpenSSL types, so consumers do not need the
# OpenSSL headers directly. PRIVATE is correct: the dependency is satisfied
# for compilation of ScramCrypto.cpp and the linker receives OpenSSL::Crypto,
# but it is not re-exported to consumers.
target_link_libraries(scram_crypto PRIVATE OpenSSL::Crypto)

# Header listed explicitly so CLion indexes it correctly.
target_sources(scram_crypto PRIVATE ScramCrypto.hpp)
