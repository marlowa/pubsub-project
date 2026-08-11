#! /bin/sh
rm -f clang-output.txt clang-summary.txt clang-summary-byfile.txt
rm -fr build # hack until exclude-dirs works

PROJECT_VERSION=$(sed -n 's/.*VERSION \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' CMakeLists.txt | head -1)
mkdir -p build/generated_build_info/pubsub_itc_fw
cmake -DSOURCE_DIR="$(pwd)" \
      -DOUTPUT_DIR="$(pwd)/build/generated_build_info" \
      -DPROJECT_VERSION="${PROJECT_VERSION}" \
      -P cmake/GenerateBuildInfo.cmake \
      || { echo "Error: BuildInfo.hpp generation failed."; exit 1; }

mkdir -p build/generated_dsl
python3 python/tools/generate_cpp_from_dsl.py --topics --namespace pubsub_itc_fw_app \
        applications/fix_equity_orders.dsl build/generated_dsl/fix_equity_orders.hpp \
        || { echo "Error: DSL generation failed (fix_equity_orders)."; exit 1; }
python3 python/tools/generate_cpp_from_dsl.py --namespace pubsub_itc_fw_app \
        applications/authentication.dsl build/generated_dsl/authentication.hpp \
        || { echo "Error: DSL generation failed (authentication)."; exit 1; }
python3 python/tools/generate_cpp_from_dsl.py --namespace pubsub_itc_fw_app \
        libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl \
        build/generated_dsl/leader_follower.hpp \
        || { echo "Error: DSL generation failed (leader_follower app)."; exit 1; }

mkdir -p build/libraries/pubsub_itc_fw/dsl
python3 python/tools/generate_cpp_from_dsl.py --namespace pubsub_itc_fw \
        libraries/pubsub_itc_fw/include/pubsub_itc_fw/leader_follower.dsl \
        build/libraries/pubsub_itc_fw/dsl/leader_follower.hpp \
        || { echo "Error: DSL generation failed (leader_follower fw)."; exit 1; }
python3 python/tools/generate_cpp_from_dsl.py --namespace pubsub_itc_fw \
        libraries/pubsub_itc_fw/integration_tests/variable_length_test_protocol.dsl \
        build/libraries/pubsub_itc_fw/dsl/variable_length_test_protocol.hpp \
        || { echo "Error: DSL generation failed (variable_length_test_protocol)."; exit 1; }
python3 python/tools/generate_cpp_from_dsl.py --namespace pubsub_itc_fw \
        libraries/pubsub_itc_fw/performance/DslBenchProtocol.dsl \
        build/libraries/pubsub_itc_fw/dsl/DslBenchProtocol.hpp \
        || { echo "Error: DSL generation failed (DslBenchProtocol)."; exit 1; }

python3 sca.py --includes=includes.txt --ignore_checks=ignore-checks.txt \
        --extra-macros=extra_macros.txt --exclusions=excludes.txt --batch_size=50 \
        --header_filter="$(realpath .)" 2>&1 | tee clang-output.txt
if [ $? -ne 0 ]; then
    echo Error: SCA failed.
    exit 1
fi
python3 sca-summary.py --file=clang-output.txt --output=clang-summary.txt \
        --output_byfile=clang-summary-byfile.txt
if [ $? -ne 0 ]; then
    echo Error: SCA summary failed.
    exit 1
fi
python3 sca-diff.py --previous=sca-results/clang-summary-byfile.txt --current=clang-summary-byfile.txt
rm -f compile_commands.json
