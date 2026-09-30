#!/bin/sh
# Builds the Mac simulator: tools/flapsim/build.sh && build/flapsim "HELLO"
set -e
cd "$(dirname "$0")/../.."
mkdir -p build
c++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Ilib/flapcore/include \
  lib/flapcore/src/*.cpp tools/flapsim/main.cpp -o build/flapsim
