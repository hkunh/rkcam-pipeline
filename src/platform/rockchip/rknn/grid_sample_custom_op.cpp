#include "rkcam/platform/rockchip/rknn/grid_sample_custom_op.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "rknn_custom_op.h"

namespace rkcam::platform::rockchip::rknn {
namespace{
const char* tensorTypeToString(rknn_tensor_type type) noexcept
{
    switch (type) {

    case RKNN_TENSOR_FLOAT32:
        return "FLOAT32";

    case RKNN_TENSOR_FLOAT16:
        return "FLOAT16";

    case RKNN_TENSOR_INT8:
        return "INT8";

    case RKNN_TENSOR_UINT8:
        return "UINT8";

    case RKNN_TENSOR_INT16:
        return "INT16";

    case RKNN_TENSOR_UINT16:
        return "UINT16";

    case RKNN_TENSOR_INT32:
        return "INT32";

    case RKNN_TENSOR_UINT32:
        return "UINT32";

    case RKNN_TENSOR_INT64:
        return "INT64";

    case RKNN_TENSOR_BOOL:
        return "BOOL";

    default:
        return "UNKNOWN";
    }
}
// ============================================================
// Tensor format helpers
// ============================================================
const char* tensorFormatToString(rknn_tensor_format format) noexcept
{
    switch (format) {

    case RKNN_TENSOR_NCHW:
        return "NCHW";

    case RKNN_TENSOR_NHWC:
        return "NHWC";

    case RKNN_TENSOR_NC1HWC2:
        return "NC1HWC2";

    case RKNN_TENSOR_UNDEFINED:
        return "UNDEFINED";

    default:
        return "UNKNOWN";
    }
}
// ============================================================
// Print shape
// ============================================================
void printTensorShape(const rknn_tensor_attr& attr)
{
    std::cerr << "[";
    for(std::uint32_t i = 0; i < attr.n_dims; ++i)
    {
        std::cerr << attr.dims[i];
        if(i + 1 < attr.n_dims)
        {
            std::cerr << ", ";
        }
    }
    std::cerr << "]";
}

// ============================================================
// Print one custom-op tensor
// ============================================================
void printTensor(
    const char* kind,
    std::uint32_t index,
    const rknn_custom_op_tensor& tensor
)
{
    const auto& attr = tensor.attr;
    const auto& mem = tensor.mem;
    
    std::cerr
        << "    "
        << kind
        << "[" << index << "]" << "\n";
    std::cerr
        << "    name    :"
        << attr.name
        << "\n";
    std::cerr
        << "    dims    :";
    printTensorShape(attr);
    std::cerr << "\n";
    std::cerr
        << "    n_dims  :"
        << attr.n_dims
        << "\n";
    std::cerr
        << "    format  :"
        << tensorFormatToString(attr.fmt)
        << "\n";
    std::cerr
        << "    type    :"
        << tensorTypeToString(attr.type)
        << "\n";
    std::cerr
        << "    elements    :"
        << attr.n_elems
        << "\n";
    std::cerr
        << "    attr.size   :"
        << attr.size
        << "bytes"
        << "\n";
    std::cerr
        << "    size_with_stride :"
        << attr.size_with_stride
        << "bytes"
        << "\n";
    // --------------------------------------------------------
    // custom-op callback 真正访问数据时，
    // 后面就是从：
    //
    //   mem.virt_addr + mem.offset
    //
    // 得到 tensor 的实际 CPU 地址。
    //
    // 现在只打印，不访问。
    // --------------------------------------------------------
    std::cerr
        << "    mem.virt_addr   :"
        << mem.virt_addr
        << "\n";
    std::cerr
        << "    mem.offset      :"
        << mem.offset
        << "\n";
    std::cerr
        << "    mem.size        :"
        << mem.size
        << " bytes"
        << "\n";
    std::cerr
        << "    mem.fd          :"
        << mem.fd
        << "\n";
}




// ============================================================
// GridSample probe callback
// ============================================================
//
// IMPORTANT:
//
// 当前绝对不进行任何 GridSample 运算。
//
// 目的只有：
//
//   1. 确认 callback 被 Runtime 调用
//   2. 查看 Runtime 实际传进来的 tensor layout
//   3. 查看实际 dtype
//   4. 查看实际 dims
//   5. 查看 output dims
//
// 如果 callback 能运行到这里，说明：
//
//   rknn_register_custom_ops()
//           ↓
//   op_type = GridSample
//           ↓
//   原模型 GridSample
//
// 这条绑定关系成立。
//
// ===========================================================
int gridSampleComputeProbe(
    rknn_custom_op_context* op_ctx,
    rknn_custom_op_tensor* inputs,
    std::uint32_t n_inputs,
    rknn_custom_op_tensor* outputs,
    std::uint32_t n_outputs
)
{
    (void)op_ctx;
    static std::atomic<std::uint64_t>call_count{0};
    const std::uint64_t call_index = ++call_count;
    std::cerr << "\n";
    std::cerr
        << "============================================================"
        << "\n";
    std::cerr
        << "[GridSampleCustomOp] compute callback invoked"
        << "\n";
    std::cerr
        << "[GridSampleCustomOp] call #"
        << call_index
        << "\n";
    std::cerr
        << "============================================================"
        << "\n";
    std::cerr
        << "  n_inputs  : "
        << n_inputs
        << "\n";
    std::cerr
        << "  n_outputs : "
        << n_outputs
        << "\n";
    // --------------------------------------------------------
    // GridSample 理论应该：
    //
    // input[0]:
    //   feature
    //
    // input[1]:
    //   grid
    //
    // output[0]:
    //   sampled feature
    //
    // 但现在不做任何假设，
    // 完全按照 Runtime 实际给出的信息打印。
    // --------------------------------------------------------
    if (inputs == nullptr) {

        std::cerr
            << "[GridSampleCustomOp] ERROR: "
            << "inputs == nullptr"
            << "\n";

        return RKNN_ERR_FAIL;
    }


    if (outputs == nullptr) {

        std::cerr
            << "[GridSampleCustomOp] ERROR: "
            << "outputs == nullptr"
            << "\n";

        return RKNN_ERR_FAIL;
    }


    for (std::uint32_t i = 0;
         i < n_inputs;
         ++i) {

        printTensor(
            "input",
            i,
            inputs[i]);
    }


    for (std::uint32_t i = 0;
         i < n_outputs;
         ++i) {

        printTensor(
            "output",
            i,
            outputs[i]);
    }


    // --------------------------------------------------------
    // 这里故意返回失败。
    //
    // 第一阶段 probe 不能写 output buffer。
    //
    // 理想行为：
    //
    //   rknn_run()
    //      ↓
    //   调用这个 callback
    //      ↓
    //   callback 返回 -1
    //      ↓
    //   rknn_run() 返回失败
    //
    // 而不是继续使用未初始化 output。
    // --------------------------------------------------------

    std::cerr
        << "[GridSampleCustomOp] probe finished."
        << "\n";

    std::cerr
        << "[GridSampleCustomOp] "
        << "Returning RKNN_ERR_FAIL intentionally."
        << "\n";

    std::cerr
        << "============================================================"
        << "\n";

    std::cerr.flush();


    return RKNN_ERR_FAIL;
}
template <typename T>
T* tensorData(rknn_custom_op_tensor& tensor) noexcept
{
    if(tensor.mem.virt_addr == nullptr)
    {
        return nullptr;
    }
    auto* base = static_cast<std::uint8_t*>(tensor.mem.virt_addr);
    return reinterpret_cast<T*>(base + tensor.mem.offset);

}
template <typename T>
const T* tensorData(const rknn_custom_op_tensor& tensor) noexcept
{
    if (tensor.mem.virt_addr == nullptr) {
        return nullptr;
    }
    const auto* base = static_cast<const std::uint8_t*>(tensor.mem.virt_addr);
    return reinterpret_cast<const T*>(base + tensor.mem.offset);
}

// ============================================================
// Basic validation
// ============================================================
bool validateGridSampleTensors(
    const rknn_custom_op_tensor& feature,
    const rknn_custom_op_tensor& grid,
    const rknn_custom_op_tensor& output
) noexcept
{
    // --------------------------------------------------------
    // 当前实现只支持：
    //
    // feature:
    //   FLOAT32
    //   [N,C,H,W]
    //
    // grid:
    //   FLOAT32
    //   [N,Hout,Wout,2]
    //
    // output:
    //   FLOAT32
    //   [N,C,Hout,Wout]
    //
    // 这正好对应我们 probe 得到的实际 RKNN callback 格式。
    // --------------------------------------------------------
    if (feature.attr.type != RKNN_TENSOR_FLOAT32 ||
        grid.attr.type != RKNN_TENSOR_FLOAT32 ||
        output.attr.type != RKNN_TENSOR_FLOAT32) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "only FLOAT32 callback tensors "
            << "are currently supported\n";

        return false;
    }


    if (feature.attr.n_dims != 4 ||
        grid.attr.n_dims != 4 ||
        output.attr.n_dims != 4) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "all tensors must be rank 4\n";

        return false;
    }


    const std::uint32_t n =
        feature.attr.dims[0];

    const std::uint32_t c =
        feature.attr.dims[1];


    // GridSample 的 grid 语义不是普通 NCHW。
    //
    // ONNX:
    //
    //   grid = [N, Hout, Wout, 2]
    //
    const std::uint32_t grid_n =
        grid.attr.dims[0];

    const std::uint32_t out_h =
        grid.attr.dims[1];

    const std::uint32_t out_w =
        grid.attr.dims[2];

    const std::uint32_t coord_dim =
        grid.attr.dims[3];


    if (coord_dim != 2) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "grid last dimension must be 2\n";

        return false;
    }


    if (grid_n != n) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "feature/grid batch mismatch\n";

        return false;
    }


    // output:
    //
    // [N,C,Hout,Wout]

    if (output.attr.dims[0] != n ||
        output.attr.dims[1] != c ||
        output.attr.dims[2] != out_h ||
        output.attr.dims[3] != out_w) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "output shape mismatch\n";

        return false;
    }
    return true;
}

// ============================================================
// Custom op object
// ============================================================
//
// 使用 static storage。
// 不把临时局部 rknn_custom_op 的地址交给 Runtime。
// 向RKNN Runtime注册GridSample的算子
// ============================================================
rknn_custom_op& getGridSampleCustomOp() noexcept
{
    static rknn_custom_op custom_op{};
    static const bool initialized = [](){
        std::memset(&custom_op, 0, sizeof(custom_op));
        std::strncpy(custom_op.op_type, "GridSample", RKNN_MAX_NAME_LEN - 1);
        custom_op.op_type[RKNN_MAX_NAME_LEN - 1] = '\0';
        custom_op.version = 1;
        custom_op.target = RKNN_TARGET_TYPE_CPU;
        //optional
        custom_op.init = nullptr;
        //optional
        custom_op.prepare = nullptr;
        //required
        custom_op.compute = gridSampleComputeProbe;
        // 官方 header 当前说明：
        // compute_native 暂不支持，应设 nullptr。
        custom_op.compute_native = nullptr;
        //optional
        custom_op.destroy = nullptr;
        return true;
    }(); //最后()是立即调用
    (void) initialized; //避免编译器因为没有使用initialized而警告
    return custom_op;
}

} //namespace


// ============================================================
// Public registration API
// ============================================================

int registerGridSampleCustomOp(rknn_context context) noexcept
{
    if(context == 0)
    {
        std::cerr
            << "[GripSampleCustomOp] "
            << "invalid RKNN context"
            << "\n";
        return RKNN_ERR_CTX_INVALID;
    }
    rknn_custom_op& custom_op = getGridSampleCustomOp();
    std::cerr
        << "[GridSampleCustomOp] "
        << "registering op_type=\""
        << custom_op.op_type
        << "\" target=CPU"
        << "\n";
    const int ret =
        rknn_register_custom_ops(
            context,
            &custom_op,
            1);
    if (ret != RKNN_SUCC) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "rknn_register_custom_ops failed, ret="
            << ret
            << "\n";

        return ret;
    }
    std::cerr
        << "[GridSampleCustomOp] "
        << "registration succeeded"
        << "\n";

    return RKNN_SUCC;
}


}//namespace rkcam::platform::rockchip::rknn