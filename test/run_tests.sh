#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p test/build

echo "[1/2] protocol/format/battery tests"
g++ -std=c++11 -Wall -Wextra -Iinclude -Isrc \
    test/test_protocol.cpp src/protocol_format.cpp src/battery.cpp src/settings.cpp \
    -o test/build/test_protocol
./test/build/test_protocol

echo "[2/2] communication parser tests"
g++ -std=c++11 -Wall -Wextra -Itest/stubs -Iinclude -Isrc \
    test/test_comm.cpp src/comm.cpp src/protocol_format.cpp src/settings.cpp \
    -o test/build/test_comm
./test/build/test_comm

echo
echo "All host tests passed."
