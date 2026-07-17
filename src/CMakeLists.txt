add_library(rkcam_core
    core/log.cpp
    core/timestamp.cpp
    core/blocking_queue.cpp
)
target_include_directories(rkcam_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
add_library(rkcam::core ALIAS rkcam_core)


add_library(rkcam_video_source
    video/v4l2_video_source.cpp
)
target_link_libraries(rkcam_video_source PUBLIC
    rkcam::core
)
add_library(rkcam::video_source ALIAS rkcam_video_source)

add_library(rkcam_video_frame
    video/video_frame.cpp
)
target_link_libraries(rkcam_video_frame PUBLIC
    rkcam::core
)
add_library(rkcam::video_frame ALIAS rkcam_video_frame)

# =========================
# MPP
# =========================
set(MPP_LIB "$ENV{RK_SYSROOT}/usr/lib/librockchip_mpp.so")
if(NOT EXISTS "${MPP_LIB}")
    message(FATAL_ERROR "MPP library not found: ${MPP_LIB}")
endif()

add_library(mpp::mpp SHARED IMPORTED)
set_target_properties(mpp::mpp PROPERTIES
    IMPORTED_LOCATION "${MPP_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/include/rkcam/platform/rockchip/mpp/mpp_rk3568"
)

add_library(rkcam_mpp_processor
    platform/rockchip/mpp/mpp_buffer_pool.cpp
    platform/rockchip/mpp/mpp_encoder.cpp
)
target_include_directories(rkcam_mpp_processor PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(rkcam_mpp_processor PUBLIC
    rkcam::core
    rkcam::pipeline
    mpp::mpp
)
add_library(rkcam::mpp_processor ALIAS rkcam_mpp_processor)


# =========================
# RGA
# =========================
set(RGA_LIB "$ENV{RK_SYSROOT}/usr/lib/librga.so")

if(NOT EXISTS "${RGA_LIB}")
    message(FATAL_ERROR "RGA library not found: ${RGA_LIB}")
endif()
add_library(rga::rga SHARED IMPORTED)
set_target_properties(rga::rga PROPERTIES
    IMPORTED_LOCATION "${RGA_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/include/rkcam/platform/rockchip/rga/rga_rk3568"
)

add_library(rkcam_rga_processor
    platform/rockchip/rga/rga_processor.cpp
)
target_include_directories(rkcam_rga_processor PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(rkcam_rga_processor PUBLIC
    rkcam::core
    rkcam::pipeline
    rga::rga
)
add_library(rkcam::rga_processor ALIAS rkcam_rga_processor)




# =========================
# drm
# =========================

set(DRM_LIB "$ENV{RK_SYSROOT}/usr/lib/libdrm.so")
set(DRM_INC "$ENV{RK_SYSROOT}/usr/include/libdrm")
if(NOT EXISTS "${DRM_LIB}")
    message(FATAL_LIB "DRM library not found: ${DRM_LIB}")
endif()

add_library(drm::drm SHARED IMPORTED)
set_target_properties(drm::drm PROPERTIES
    IMPORTED_LOCATION "${DRM_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${DRM_INC};${RK_SYSROOT}/usr/include"
)

add_library(rkcam_drm 
    platform/linux/drm/drm_device.cpp
    platform/linux/drm/drm_buffer_pool.cpp
    platform/linux/drm/drm_display_sink.cpp
)
target_include_directories(rkcam_drm PUBLIC
    ${CMAKE_SOURCE_DIR}/inclue
)
target_link_libraries(rkcam_drm PUBLIC
    rkcam::core
    drm::drm
)

add_library(rkcam::drm ALIAS rkcam_drm)


# =========================
# FFmpeg
# =========================
find_library(AVFORMAT_LIB avformat
    PATHS
        "$ENV{SYSROOT}/usr/lib"
        "$ENV{SYSROOT}/usr/lib64"
    NO_DEFAULT_PATH
)

find_library(AVCODEC_LIB avcodec
    PATHS
        "$ENV{SYSROOT}/usr/lib"
        "$ENV{SYSROOT}/usr/lib64"
    NO_DEFAULT_PATH
)

find_library(AVUTIL_LIB avutil
    PATHS
        "$ENV{SYSROOT}/usr/lib"
        "$ENV{SYSROOT}/usr/lib64"
    NO_DEFAULT_PATH
)

if(NOT AVFORMAT_LIB OR NOT AVCODEC_LIB OR NOT AVUTIL_LIB)
    message(FATAL_ERROR "FFmpeg libraries not found in RK_SYSROOT")
endif()

add_library(ffmpeg::avformat SHARED IMPORTED)
set_target_properties(ffmpeg::avformat PROPERTIES
    IMPORTED_LOCATION "${AVFORMAT_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "$ENV{SYSROOT}/usr/include"
)

add_library(ffmpeg::avcodec SHARED IMPORTED)
set_target_properties(ffmpeg::avcodec PROPERTIES
    IMPORTED_LOCATION "${AVCODEC_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "$ENV{SYSROOT}/usr/include"
)

add_library(ffmpeg::avutil SHARED IMPORTED)
set_target_properties(ffmpeg::avutil PROPERTIES
    IMPORTED_LOCATION "${AVUTIL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "$ENV{SYSROOT}/usr/include"
)




add_library(rkcam_pipeline
    pipeline/capture_stage.cpp
    pipeline/fps_stage.cpp
    pipeline/raw_save_stage.cpp
    pipeline/camera_pipeline.cpp
    pipeline/rga_stage.cpp
    pipeline/mpp_stage.cpp
    pipeline/encoded_save_stage.cpp
    pipeline/mp4_record_stage.cpp
    pipeline/display_stage.cpp
    pipeline/videoframe_stage.cpp
    pipeline/encoded_packet_tee_stage.cpp
    pipeline/rtsp_push_stage.cpp
)
target_link_libraries(rkcam_pipeline PUBLIC
    rkcam::core
    rkcam::video_frame
    rkcam::video_source
    rkcam::mpp_processor
    ffmpeg::avformat
    ffmpeg::avcodec
    ffmpeg::avutil
    rkcam::drm
)
add_library(rkcam::pipeline ALIAS rkcam_pipeline)



