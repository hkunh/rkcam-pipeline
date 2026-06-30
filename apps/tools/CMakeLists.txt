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



add_executable(mp4_mux_h264_test
    mp4_mux_h264_test.cpp
)
target_include_directories(mp4_mux_h264_test PRIVATE
    "$ENV{RK_SYSROOT}/usr/include"
)
target_link_libraries(mp4_mux_h264_test PRIVATE
    "$ENV{RK_SYSROOT}/usr/lib/libavformat.so"
    "$ENV{RK_SYSROOT}/usr/lib/libavcodec.so"
    "$ENV{RK_SYSROOT}/usr/lib/libavutil.so"
)
set_target_properties(mp4_mux_h264_test PROPERTIES
    BUILD_RPATH "\$ORIGIN/../lib"
    INSTALL_RPATH "\$ORIGIN/../lib"
    BUILD_WITH_INSTALL_RPATH TRUE
)