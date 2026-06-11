add_executable(rkcamd
	main.cpp
)

target_link_libraries(rkcamd PRIVATE
	rkcam::core
	rkcam::pipeline
	rkcam::video_source
)
