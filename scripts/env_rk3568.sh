##################################################### #####
# File Name: scripts/env_rk3568.sh
# Author:HKH368

# Created Time: 2026-05-30
# brief 
###########################################################

#!/bin/bash
export RK_SDK=$HOME/xuexi/rk3568/sdk/rk3568_linux_sdk
export RK_TOOLCHAIN=$RK_SDK/buildroot/output/rockchip_rk3568/host
export RK_TOOLCHAIN_PREFIX=aarch64-buildroot-linux-gnu
export RK_SYSROOT=$RK_TOOLCHAIN/aarch64-buildroot-linux-gnu/sysroot

export PATH=$RK_TOOLCHAIN/bin:$PATH
export LD_LIBRARY_PATH=$RK_TOOLCHAIN/lib:$LD_LIBRARY_PATH

echo "RK_SDK=$RK_SDK"
echo "RK_TOOLCHAIN=$RK_TOOLCHAIN"
echo "RK_TOOLCHAIN_PREFIX=$RK_TOOLCHAIN_PREFIX"
echo "RK_SYSROOT=$RK_SYSROOT"
