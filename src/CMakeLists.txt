add_library(rkcam_core
	core/log.cpp
	core/timestamp.cpp
	core/blocking_queue.cpp
	)

target_include_directories(rkcam_core PUBLIC 
	${CMAKE_SOURCE_DIR}/include
	)

add_library(rkcam_video_source
	video/v4l2_video_source.cpp
	)

target_link_libraries(rkcam_video_source PUBLIC
	rkcam_core
	)

add_library(rkcam_pipeline_lib 
	pipeline/pipeline.cpp
	)

target_link_libraries(rkcam_pipeline_lib PUBLIC
	rkcam_core
	rkcam_video_source
	)

