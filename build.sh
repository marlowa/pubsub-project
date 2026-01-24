#! /bin/bash
export THIRDPARTY_DIR=/home/marlowa/mystuff/thirdparty
export PROTOBUF_VERSION=25.1
export ABSEIL_VERSION=abseil-cpp-20230802.1
export FMT_VERSION=12.1.0
export QUILL_VERSION=11.0.2
export ARGPARSE_VERSION=3.2
export GOOGLETEST_VERSION=1.17.0

if [ ! -d build ]; then
  mkdir build
fi
cd build
cmake ..

