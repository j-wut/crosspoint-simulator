#!/bin/sh
# Checks the FreeRTOS semaphore shim against the device's rules.
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/sim-freertos.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT
for variant in normal ndebug; do
  variant_flag=""
  if [ "$variant" = ndebug ]; then
    variant_flag="-DNDEBUG"
  fi
  c++ -std=c++20 -g -Wall -pthread $variant_flag -Isrc \
    tests/freertos-locks.cpp src/freertos/semphr.cpp src/SimLog.cpp \
    -o "$build_dir/$variant"
  "$build_dir/$variant" 2>/dev/null
  for must_abort in give-other null-take; do
    if "$build_dir/$variant" "$must_abort" 2>/dev/null; then
      echo "$must_abort did not abort" >&2
      exit 1
    fi
  done
  c++ -std=c++20 -Wall -pthread $variant_flag -Isrc \
    tests/static-mutex.cpp src/freertos/semphr.cpp src/SimLog.cpp \
    -o "$build_dir/mutex-$variant"
  "$build_dir/mutex-$variant"
done
