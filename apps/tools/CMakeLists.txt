add_executable(v4l2_capture_test
	v4l2_capture_test.cpp
)
target_link_libraries(v4l2_capture_test PRIVATE
    rkcam::core
    rkcam::video_source
)

add_executable(v4l2_dma_export_test
    v4l2_dma_export_test.cpp
)
target_link_libraries(v4l2_dma_export_test PRIVATE
    rkcam::core
    rkcam::video_source
)

add_executable(rga_basic_test
    rga_basic_test.cpp
)
target_link_libraries(rga_basic_test PRIVATE
    rkcam::core
    rkcam::rga_processor
)

add_executable(rga_process_test
    rga_process_test.cpp
)
target_link_libraries(rga_process_test PRIVATE
    rkcam::core
    rkcam::video_source
    rkcam::pipeline
    rkcam::rga_processor
)


add_executable(mpp_encoder_test
    mpp_encoder_test.cpp
)
target_link_libraries(mpp_encoder_test PRIVATE
    rkcam::core
    rkcam::video_source
    rkcam::rga_processor
    rkcam::mpp_processor
)