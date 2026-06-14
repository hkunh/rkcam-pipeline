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
)

target_link_libraries(rkcam_pipeline PUBLIC
    rkcam::core
    rkcam::video_source
)

add_library(rkcam::pipeline ALIAS rkcam_pipeline)