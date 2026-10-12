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


add_executable(
    rknn_rgbt_hit_test
    rknn_rgbt_hit_test.cpp
)
target_link_libraries(
    rknn_rgbt_hit_test
    PRIVATE
        rkcam_ai_unalignedHit
)
add_executable(
    onnx_rgbt_hit_test
    onnx_rgbt_hit_test.cpp
)
target_link_libraries(
    onnx_rgbt_hit_test
    PRIVATE
        ONNX::Runtime
        rkcam_ai
        rkcam_ai_unalignedHit
)

add_executable(
    hit_onnx_rknn_compare_generic
    hit_onnx_rknn_compare_generic.cpp
)

target_link_libraries(
    hit_onnx_rknn_compare_generic
    PRIVATE
    rkcam_ai
    ONNX::Runtime
)

# Board-local PMATrack manifest-driven RKNN pipeline benchmark (no ONNX link).
add_executable(
    rknn_pipeline_bench
    rknn_pipeline_bench.cpp
    pipeline_bench_common.cpp
)
target_include_directories(
    rknn_pipeline_bench
    PRIVATE "${PROJECT_SOURCE_DIR}/third_party"
)
target_link_libraries(rknn_pipeline_bench PRIVATE rkcam_ai)
