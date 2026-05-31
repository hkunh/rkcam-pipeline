##################################################### #####
# File Name: build_host.sh
# Author:HKH368

# Created Time: 2026-05-28
# brief 
###########################################################

#!/bin/bash
set -e
ROOT_DIR=$(cd "$(dirname $0)/.." && pwd)
BUILD_DIR="$ROOT_DIR/build-host"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
	-DCMAKE_BUILD_TYPE=Debug \
	-G Ninja
cmake --build "$BUILD_DIR" -j$(nproc)
