##################################################### #####
# File Name: build_board.sh
# Author:HKH368

# Created Time: 2026-05-28
# brief 
###########################################################

#!/bin/bash
ROOT_DIR=$(cd "$(dirname $0)/.." && pwd)
BUILD_DIR="$ROOT_DIR/build-rk3568"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchains/aarch64-linux-gnu.cmake" \
	-DCMAKE_BUILD_TYPE=Release \
	-G Ninja
cmake --build "$BUILD_DIR" -j$(nproc)
