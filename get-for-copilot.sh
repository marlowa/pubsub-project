#! /bin/bash
for i in $(find . \( -name '*.hpp' -o -name '*.cpp' \))
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
