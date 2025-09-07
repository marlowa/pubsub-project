#! /bin/sh
rm -f clang-output.txt clang-summary.txt clang-summary-byfile.txt
rm -fr build # hack until exclude-dirs works
python3 sca.py --includes=includes.txt --ignore_checks=ignore-checks.txt --exclusions=excludes.txt 2>&1 | tee clang-output.txt
if [ $? -ne 0 ]; then
    echo Error: SCA failed.
    exit 1
fi
python3 sca-summary.py --file=clang-output.txt --output=clang-summary.txt --output_byfile=clang-summary-byfile.txt
if [ $? -ne 0 ]; then
    echo Error: SCA summary failed.
    exit 1
fi
python3 sca-diff.py --previous=sca-results/clang-summary-byfile.txt --current=clang-summary-byfile.txt
