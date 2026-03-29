#! /bin/bash
for i in $(find . \( -name '*.hpp' -o -name '*.cpp' -o -name '*.py' -o -name '*.sh' \))
do
  f=$(basename $i)
  cp $i for-copilot/${f}.txt
done
echo Copied source files to txt for copilot as of `date`.
for i in build.sh build.py Doxyfile docs/design_overview.md
do
  cp $i for-copilot/${i}.txt
done
echo Copied misc files to txt for copilot as of `date`.
cp CMakeLists.txt for-copilot
cp ./libraries/pubsub_itc_fw/CMakeLists.txt for-copilot/CMakeLists-library.txt
cp ./libraries/pubsub_itc_fw/tests/CMakeLists.txt for-copilot/CMakeLists-tests.txt
cp ./libraries/pubsub_itc_fw/performance/CMakeLists.txt for-copilot/CMakeLists-performance.txt
echo Copied cmake files to txt for copilot as of `date`.
