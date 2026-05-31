##################################################### #####
# File Name: deploy.sh
# Author:HKH368

# Created Time: 2026-05-28
# brief 
###########################################################

#!/bin/bash
#!/bin/bash
set -e

BOARD_IP=${BOARD_IP:-192.168.1.100}
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)

ssh root@$BOARD_IP "mkdir -p /userdata/rkcam/bin /userdata/rkcam/configs"

scp "$ROOT_DIR/build-rk3568/apps/tools/v4l2_capture_test" \
    root@$BOARD_IP:/userdata/rkcam/bin/

scp "$ROOT_DIR/build-rk3568/apps/rkcamd/rkcamd" \
    root@$BOARD_IP:/userdata/rkcam/bin/

scp -r "$ROOT_DIR/configs" \
    root@$BOARD_IP:/userdata/rkcam/
