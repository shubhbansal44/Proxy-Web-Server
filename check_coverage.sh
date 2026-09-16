#!/bin/bash

# This script parses the gcov output and fails if coverage is below 80%

echo "Checking coverage for core modules..."
COVERAGE_MET=1

for file in proxy_parse.c.gcov config.c.gcov cache.c.gcov; do
    if [ ! -f "$file" ]; then
        echo "Error: $file not found!"
        exit 1
    fi
    
    # Extract the percentage
    # gcov outputs something like: Lines executed:81.25% of 320
    # Or we can just read it from the gcov file? Wait, gcov writes a .gcov file but output to stdout has the percentage.
    # It's easier to run gcov again and parse it.
done

echo "Running gcov to parse percentages..."
# gcov proxy_parse.c config.c
for src_file in proxy_parse.c config.c cache.c; do
    OUTPUT=$(gcov $src_file)
    echo "$OUTPUT"
    
    # Extract percentage, e.g. "Lines executed:81.25% of 320" -> "81.25"
    PCT=$(echo "$OUTPUT" | grep "Lines executed:" | head -n 1 | awk -F':' '{print $2}' | awk -F'%' '{print $1}')
    
    if [ -z "$PCT" ]; then
        echo "Could not parse coverage for $src_file"
        exit 1
    fi
    
    # Use awk for float comparison
    IS_PASS=$(echo "$PCT" | awk '{if ($1 >= 80.0) print 1; else print 0}')
    
    if [ "$IS_PASS" -eq 1 ]; then
        echo "$src_file PASSED with $PCT%"
    else
        echo "$src_file FAILED with $PCT% (requires 80.0%)"
        COVERAGE_MET=0
    fi
done

if [ "$COVERAGE_MET" -eq 0 ]; then
    echo "One or more core modules failed the coverage requirement!"
    exit 1
else
    echo "All core modules met the coverage requirement!"
    exit 0
fi
