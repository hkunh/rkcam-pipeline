add_executable(
    rknn_rgbt_rtdetr_test
    rknn_rgbt_rtdetr_test.cpp
)
target_link_libraries(
    rknn_rgbt_rtdetr_test
    PRIVATE
        rkcam_ai
)

add_executable(
    onnx_rgbt_rtdetr_test
    onnx_rgbt_rtdetr_test.cpp
)
target_link_libraries(
    onnx_rgbt_rtdetr_test
    PRIVATE
        ONNX::Runtime
        rkcam_ai
)
set_target_properties(
    onnx_rgbt_rtdetr_test
    PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../lib"
)

add_executable(
    Hit_input_preprocessor_test
    Hit_input_preprocessor_test.cpp
)
target_link_libraries(
    Hit_input_preprocessor_test
    PRIVATE
        rkcam_ai_unalignedHit
        rkcam_ai
)
