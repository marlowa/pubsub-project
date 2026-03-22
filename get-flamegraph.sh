#! /bin/bash
./build.sh --clean --no-tests --verbose

# Only sample on CPU 2 and 3 where your threads are pinned
# Use cpu_core cycles only, not cpu_atom
perf record -C 2,3 -e cpu_core/cycles/ --call-graph fp -F 9999 \
    ./build/libraries/pubsub_itc_fw/performance/fixed_pool_bench

perf script 2>/dev/null | ~/mystuff/FlameGraph/stackcollapse-perf.pl | \
    ~/mystuff/FlameGraph/flamegraph.pl > flamegraph.svg

convert flamegraph.svg flamegraph.jpg
