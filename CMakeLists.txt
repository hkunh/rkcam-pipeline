cmake_minimum_required(VERSION 3.16)

project(rkcam_pipeline LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_RK_MPP "Enable Rockchip MPP" ON)
option(ENABLE_RK_RGA "Enable Rockchip RGA" ON)
option(ENABLE_RKNN "Enable RKNN" OFF)
option(ENABLE_RTSP "Enable RTSP" OFF)
option(ENABLE_UI "Enable local UI" OFF)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_subdirectory(src)
add_subdirectory(apps)
