add_library(
    rkcam_ai
    STATIC
    image_loader.cpp
    rknn_engine.cpp
    rgbt_preprocessor.cpp
    rtdetr_postprocessor.cpp
)
target_include_directories(
    rkcam_ai 
    PUBLIC 
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE
        "${PROJECT_SOURCE_DIR}/third_party/stb"    
)
target_link_libraries(
    rkcam_ai
    PUBLIC 
        RKNN::Runtime
    PRIVATE 
        rkcam_rknn_custom_ops
)
add_library(rkcam::ai ALIAS rkcam_ai)

add_subdirectory(
    unaligned_hit
)