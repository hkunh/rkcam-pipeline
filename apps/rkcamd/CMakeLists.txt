add_executable(rkcamd
	main.cpp
	pipeline_config.cpp
	camera_service.cpp
)

target_link_libraries(rkcamd PRIVATE
	rkcam::core
	rkcam::pipeline
	rkcam::video_source
	rkcam::rga_processor
	rkcam::drm
)
