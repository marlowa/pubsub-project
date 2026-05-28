wget https://github.com/fmtlib/fmt/archive/refs/tags/11.0.2.tar.gz
tar xzf 11.0.2.tar.gz
cd fmt-11.0.2
cmake -S . -B build \
    -DCMAKE_INSTALL_PREFIX=/workspace/thirdparty/installed/fmt/11.0.2 \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DFMT_TEST=OFF
cmake --build build --parallel $(nproc)
cmake --install build
