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



add_library(rkcam_pipeline
    pipeline/pipeline.cpp
    pipeline/pipeline_video_frame.cpp
    pipeline/capture_stage.cpp
    pipeline/fps_stage.cpp
    pipeline/raw_save_stage.cpp
    pipeline/camera_pipeline.cpp
    pipeline/rga_stage.cpp
)
target_link_libraries(rkcam_pipeline PUBLIC
    rkcam::core
    rkcam::video_source
)
add_library(rkcam::pipeline ALIAS rkcam_pipeline)


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
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/include/rkcam/video/rga_rk3568"
)

add_library(rkcam_rga_processor
    video/rga_processor.cpp
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