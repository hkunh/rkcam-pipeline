##################################################### #####
# File Name: scripts/deploy_adb.sh
# Author:HKH368

# Created Time: 2026-05-30
# brief 
###########################################################

#!/bin/bash
set -e
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build-rk3568"

adb shell "mkdir -p /userdata/rkcam/bin /userdata/rkcam/configs"

adb push "$BUILD_DIR/apps/rkcamd/rkcamd" /userdata/rkcam/bin/
adb push "$BUILD_DIR/apps/camctl/camctl" /userdata/rkcam/bin/
adb push "$BUILD_DIR/apps/tools/v4l2_capture_test" /userdata/rkcam/bin/

adb shell "chmod +x /userdata/rkcam/bin/rkcamd /userdata/rkcam/bin/camctl /userdata/rkcam/bin/v4l2_capture_test"

echo "deploy done"

