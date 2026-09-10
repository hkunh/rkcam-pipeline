

add_executable(
    rknn_rgbt_rtdetr_test
    rknn_rgbt_rtdetr_test.cpp
)
target_link_libraries(
    rknn_rgbt_rtdetr_test
    PRIVATE
        rkcam_ai
)