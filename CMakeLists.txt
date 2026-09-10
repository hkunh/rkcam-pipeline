cmake_minimum_required(VERSION 3.16)

#自动设置CMAKE_SOURCE_DIR 永远指向最顶层（根目录），不会变
#自动设置PROJECT_SOURCE_DIR 指向当前 project() 所在的目录，会跟着变
project(rkcam_pipeline LANGUAGES C CXX)


set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_MPP "Enable Rockchip MPP" ON)
option(ENABLE_RK_RGA "Enable Rockchip RGA" ON)
option(ENABLE_RKNN "Enable RKNN" OFF)
option(ENABLE_RTSP "Enable RTSP" OFF)
option(ENABLE_UI "Enable local UI" OFF)

include_directories(${CMAKE_SOURCE_DIR}/include)


# =========================
# Runtime library path
# =========================
# 部署结构要求：
#   /userdata/rkcam/bin/xxx
#   /userdata/rkcam/lib/libavformat.so.58
#
# $ORIGIN 表示当前可执行文件所在目录。
# 对 /userdata/rkcam/bin/rkcamd 来说：
#   $ORIGIN/../lib = /userdata/rkcam/lib
set(RKCAM_RUNTIME_RPATH "\$ORIGIN/../lib")
set(CMAKE_BUILD_RPATH "${RKCAM_RUNTIME_RPATH}")
set(CMAKE_INSTALL_RPATH "${RKCAM_RUNTIME_RPATH}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# ============================================================
# Third-party platform dependencies
# ============================================================
include(
    "${PROJECT_SOURCE_DIR}/cmake/modules/RKNNRuntime.cmake"
)



# ============================================================
# Project modules
# ============================================================
add_subdirectory(src)
add_subdirectory(apps)



