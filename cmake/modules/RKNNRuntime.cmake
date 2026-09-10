# ============================================================
# Rockchip RKNN Runtime
# ============================================================

set(
    RKNN_VERSION
#    "2.3.2"
    "2.4.2a15"
)
set(
    RKNN_ROOT
    "${PROJECT_SOURCE_DIR}/third_party/rknn/${RKNN_VERSION}"
)
set(
    RKNN_INCLUDE_DIR
    "${RKNN_ROOT}/include"
)

set(
    RKNN_RUNTIME_LIB
    "${RKNN_ROOT}/lib/aarch64/librknnrt.so"
)

# ------------------------------------------------------------
# Validate files
# ------------------------------------------------------------

if(NOT EXISTS "${RKNN_INCLUDE_DIR}/rknn_api.h")
    message(
        FATAL_ERROR
        "RKNN header not found: ${RKNN_INCLUDE_DIR}/rknn_api.h"
    )
endif()

if(NOT EXISTS "${RKNN_RUNTIME_LIB}")
    message(
        FATAL_ERROR
        "RKNN runtime library not found: ${RKNN_RUNTIME_LIB}"
    )
endif()

# ------------------------------------------------------------
# Imported target
# ------------------------------------------------------------
#SHARED: 决定是静态库还是动态库
#IMPORTED:告诉cmake，这个库是已经编译好的.so，不需要从cpp开始编译
#GLOBAL: add_library 创建的 target 默认有作用域限制：只能在创建它的那个目录及其子目录中使用。这里已经位于顶层，是防御性设置
if(NOT TARGET RKNN:Runtime)
    add_library(
        RKNN::Runtime
        SHARED
        IMPORTED
        GLOBAL 
    )

    set_target_properties(
        RKNN::Runtime
        PROPERTIES 
        IMPORTED_LOCATION
            "${RKNN_RUNTIME_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES
            "${RKNN_INCLUDE_DIR}"
    )
endif()


message(
    STATUS
    "RKNN Runtime ${RKNN_VERSION}: ${RKNN_RUNTIME_LIB}"
)




