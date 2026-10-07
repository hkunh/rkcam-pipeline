set(
    ONNXRUNTIME_VERSION "1.30.0"
    CACHE STRING "ONNX Runtime version"
)
set(
    ONNXRUNTIME_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/onnxruntime/${ONNXRUNTIME_VERSION}"
    CACHE PATH "ONNX Runtime root directory"
)
set(
    ONNXRUNTIME_INCLUDE_DIR
    "${ONNXRUNTIME_ROOT}/include"
)
set(
    ONNXRUNTIME_LIB_DIR
    "${ONNXRUNTIME_ROOT}/lib"
)
set(
    ONNXRUNTIME_RUNTIME_LIB
    "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so"
)
# ------------------------------------------------------------
# Validate files
# ------------------------------------------------------------

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
    message(
        FATAL_ERROR
        "ONNXRUNTIME header not found: ${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h"
    )
endif()

if(NOT EXISTS "${ONNXRUNTIME_RUNTIME_LIB}")
    message(
        FATAL_ERROR
        "ONNXRUNTIME runtime library not found: ${ONNXRUNTIME_RUNTIME_LIB}"
    )
endif()

# ------------------------------------------------------------
# Imported target
# ------------------------------------------------------------
if(NOT TARGET ONNX::Runtime)
    add_library(ONNX::Runtime SHARED IMPORTED GLOBAL)

    set_target_properties(
        ONNX::Runtime
        PROPERTIES
        IMPORTED_LOCATION             "${ONNXRUNTIME_RUNTIME_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
    )
endif()

message(
    STATUS
    "RKNN Runtime ${RKNN_VERSION}: ${RKNN_RUNTIME_LIB}"
)
