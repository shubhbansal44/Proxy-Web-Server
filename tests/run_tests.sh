#!/bin/bash

# Ensure tests/bin directory exists
mkdir -p tests/bin

echo -e "\nRunning tests..."
PASS_COUNT=0
FAIL_COUNT=0

for test_bin in tests/bin/test_*; do
    if [ -x "$test_bin" ]; then
        echo "==================================="
        echo "Running $test_bin"
        echo "==================================="
        if ./$test_bin; then
            let PASS_COUNT++
        else
            let FAIL_COUNT++
        fi
    fi
done

echo -e "\n==================================="
echo "Test summary:"
echo "Passed classes: $PASS_COUNT"
echo "Failed classes: $FAIL_COUNT"
echo "==================================="

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi
exit 0
