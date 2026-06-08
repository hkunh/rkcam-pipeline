add_executable(v4l2_capture_test
	v4l2_capture_test.cpp
)

target_link_libraries(v4l2_capture_test PRIVATE
	rkcam_core
	rkcam_video_source
)

