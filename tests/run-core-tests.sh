#!/usr/bin/env bash
set -euo pipefail
c++ -std=c++17 -Wall -Wextra -Wpedantic tests/test_core.cpp -o /tmp/ndi-multichannel-core-tests
/tmp/ndi-multichannel-core-tests
