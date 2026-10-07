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
// NCHW indexing
// ============================================================
inline std::size_t featureIndex(
    std::uint32_t n,
    std::uint32_t c, 
    std::uint32_t y,
    std::uint32_t x,
    std::uint32_t channels,
    std::uint32_t height,
    std::uint32_t width
)noexcept
{
    return (
        ((static_cast<int>(n) * channels + c) * height + y) * width + x
    );  //等价于n * channels * height * width + c * height * width + y * width + x;上面那样能提高计算效率
}
inline std::size_t outputIndex(
    std::uint32_t n,
    std::uint32_t c,
    std::uint32_t y,
    std::uint32_t x,
    std::uint32_t channels,
    std::uint32_t out_height,
    std::uint32_t out_width
)noexcept
{
    return (
        ((static_cast<int>(n) * channels
         + c) * out_height
         + y) * out_width
         + x
    );
}

// ============================================================
// Grid indexing
//
// ONNX GridSample:
//
// grid:
//   [N, Hout, Wout, 2]
//
// last dimension:
//   0 = x
//   1 = y
// ============================================================
inline std::size_t gridIndex(
    std::uint32_t n,
    std::uint32_t query_idx,
    std::uint32_t point_idx,
    std::uint32_t coord,        // 坐标0 = gx, 1 = gy
    std::uint32_t num_queries,  // = 300
    std::uint32_t num_points)   // = 4
{
    return (((std::size_t)n * num_queries + query_idx) * num_points + point_idx) * 2 + coord;
}
// ============================================================
// Boundary test
//
// padding_mode = zeros
//
// 注意：
//
// 不能把坐标 clamp 到边界。
//
// 如果四邻域中的某一个点越界，
// 只有那个点贡献 0。
// 其他仍在图内的邻点仍正常参与 interpolation。
// ============================================================
inline bool inBounds(
    std::int64_t x,
    std::int64_t y,
    std::uint32_t width,
    std::uint32_t height
)noexcept
{
    return (
        x >= 0 &&
        y >= 0 &&
        x < static_cast<std::int64_t>(width) &&
        y < static_cast<std::int64_t>(height)
    );
}
// ============================================================
// Read one feature value.
//
// padding_mode = zeros
// ============================================================
inline float readFeatureOrZero(
    const float* feature,
    std::uint32_t n,
    std::uint32_t c,
    std::int64_t y, 
    std::int64_t x,
    std::uint32_t channels,
    std::uint32_t height,
    std::uint32_t width
)noexcept
{
    if (!inBounds(
            x,
            y,
            width,
            height)) {

        return 0.0f;
    }
    const auto index = featureIndex(
        n,
        c,
        static_cast<std::uint32_t>(y),
        static_cast<std::uint32_t>(x),
        channels,
        height,
        width
    );
    return feature[index];
}


// ============================================================
// Real GridSample implementation
//
// Fixed semantics used by this RT-DETR:
//
//   mode          = bilinear
//   padding_mode  = zeros
//   align_corners = false
//
// Input:
//   feature [N,C,H,W]
//
// Grid:
//   [N,Hout,Wout,2]
//
// Output:
//   [N,C,Hout,Wout]
// ============================================================
int gridSampleCompute(
    rknn_custom_op_context* op_ctx,
    rknn_custom_op_tensor* inputs,
    std::uint32_t n_inputs,
    rknn_custom_op_tensor* outputs,
    std::uint32_t n_outputs
)
{
    if(inputs == nullptr || outputs == nullptr)
    {
        std::cerr
            << "[GridSampleCustomOp]"
            << "null input/output pointer\n";
        return RKNN_ERR_FAIL;
    }

    if (n_inputs != 2 ||
        n_outputs != 1) {

        std::cerr
            << "[GridSampleCustomOp] "
            << "expected 2 inputs and 1 output, got "
            << n_inputs
            << " inputs and "
            << n_outputs
            << " outputs\n";

        return RKNN_ERR_FAIL;
    }
    const auto& feature_tensor = inputs[0];
    const auto& grid_tensor = inputs[1];
    auto& output_tensor = outputs[0];
    if(!validateGridSampleTensors(
        feature_tensor,
        grid_tensor,
        output_tensor
    ))
    {
        return RKNN_ERR_FAIL;
    }

    const float* feature =
        tensorData<float>(
            feature_tensor);

    const float* grid =
        tensorData<float>(
            grid_tensor);

    float* output =
        tensorData<float>(
            output_tensor);

    if (feature == nullptr ||
        grid == nullptr ||
        output == nullptr) {
        std::cerr
            << "[GridSampleCustomOp] "
            << "failed to obtain tensor memory\n";
        return RKNN_ERR_FAIL;
    }

    // ========================================================
    // Shapes
    // ========================================================
    const std::uint32_t batch =
        feature_tensor.attr.dims[0];
    const std::uint32_t channels =
        feature_tensor.attr.dims[1];
    const std::uint32_t input_height =
        feature_tensor.attr.dims[2];
    const std::uint32_t input_width =
        feature_tensor.attr.dims[3];
    // IMPORTANT:
    //
    // grid:
    // [N,query_num,point_num,2] = [N,Hout,Wout,2]
    const std::uint32_t output_height = grid_tensor.attr.dims[1];
    const std::uint32_t output_width = grid_tensor.attr.dims[2];
    // ========================================================
    // Debug call counter
    // ========================================================
    static std::atomic<std::uint64_t> call_count{0};
    const std::uint64_t call_index = ++call_count;

    std::cerr
        << "[GridSampleCustomOp] call #"
        << call_index
        << "feature=["
        << batch << ","
        << channels << ","
        << input_height << ","
        << input_width
        << "]"
        << "grid=["
        << batch << ","
        << output_height << ","
        << output_width << ", 2"
        << "\n";

    // ========================================================
    // GridSample
    // ========================================================
    //
    // align_corners = false
    //
    // normalized grid coordinate gx ∈ [-1,1] ,因为模型在输入把[0, 1]变成了[-1, 1],F.grid_sample是默认的[-1, 1]的
    //
    // maps to:
    //
    //   x =
    //     ((gx + 1) * W - 1) / 2
    //
    // equivalent:
    //
    //   x =
    //     (gx + 1) * W / 2 - 0.5
    //
    //
    // Same for y.
    // ========================================================
    for(std::uint32_t n = 0; n < batch; ++n)
    {
        for(std::uint32_t oy = 0; oy < output_height; ++oy)
        {
            for(std::uint32_t ox = 0; ox < output_width; ++ox)
            {
                // --------------------------------------------
                // grid[...,0] = x
                // grid[...,1] = y
                // --------------------------------------------

                float gx = grid[
                    gridIndex(
                        n,
                        oy,
                        ox,
                        0,
                        output_height,
                        output_width
                    )
                ];
                float gy = grid[
                    gridIndex(
                        n,
                        oy,
                        ox,
                        1,
                        output_height,
                        output_width
                    )
                ];
                // PyTorch grid_sample 对 NaN grid
                // 可以视为 -1。
                //
                // 正常 RT-DETR 不应该产生 NaN，
                // 这里只做防御性处理。
                if (!std::isfinite(gx)) {
                    gx = -1.0f;
                }
                if (!std::isfinite(gy)) {
                    gy = -1.0f;
                }

                // --------------------------------------------
                // align_corners = false
                // --------------------------------------------
                const float src_x = (
                    (gx + 1.0f) * (static_cast<float>(input_width)) - 1.0f
                ) * 0.5f; //这里的-1.0f是把像素对齐中心，注意后面的*0.5f,实际就是-0.5f
                const float src_y = (
                    (gy + 1.0f) * (static_cast<float>(input_height)) - 1.0f
                ) * 0.5f;
                // --------------------------------------------
                // Four neighbours
                // --------------------------------------------
                //注意！！这里对角线的坐标组合就已经能够组成四个坐标了，不需要再算x2 x3了
                const std::int64_t x0 = static_cast<std::int64_t>(std::floor(src_x));
                const std::int64_t y0 = static_cast<std::int64_t>(std::floor(src_y));
                const std::int64_t x1 = x0 + 1;
                const std::int64_t y1 = y0 + 1;

                const float dx = src_x - static_cast<float>(x0);
                const float dy = src_y - static_cast<float>(y0);
                /*
                * ============================================================
                * 双线性插值：两种等价写法
                * ============================================================
                *
                * 四邻域：
                *     v00 ---- v01      (y0 行)
                *      |        |
                *     v10 ---- v11      (y1 行)
                *
                * dx = src_x - x0   // 采样点距左边界偏移，范围 [0,1)
                * dy = src_y - y0   // 采样点距上边界偏移，范围 [0,1)
                *
                * ------------------------------------------------------------
                * 写法 1：两次线性插值（先沿 x，再沿 y）
                * ------------------------------------------------------------
                *   top    = v00 + (v01 - v00) * dx
                *   bottom = v10 + (v11 - v10) * dx
                *   result = top + (bottom - top) * dy
                *
                * 记忆中的 (v11 - v01) * (dx / (x1 - x0)) 是这里
                * 沿 x 方向插值的中间步骤。
                *
                * ------------------------------------------------------------
                * 写法 2：四权重合并（代码采用）
                * ------------------------------------------------------------
                *   w00 = (1 - dx) * (1 - dy)
                *   w01 = dx       * (1 - dy)
                *   w10 = (1 - dx) * dy
                *   w11 = dx       * dy
                *
                *   result = v00*w00 + v01*w01 + v10*w10 + v11*w11
                *
                * ------------------------------------------------------------
                * 说明
                * ------------------------------------------------------------
                * - 两种写法数学完全等价。
                * - 写法 2 的权重只与坐标有关，与 channel 无关，
                *   可在 channel 循环外算一次，内层复用，减少重复计算。
                * - 四个权重之和 = 1，保证插值不改变整体亮度。
                * ============================================================
                */
                const float w00 = (1.0f - dx) * (1.0f - dy);
                const float w01 = dx * (1.0f - dy);
                const float w10 = (1.0f - dx) * dy;
                const float w11 = dx * dy;
                // --------------------------------------------
                // Same grid coordinate is shared by all C.
                //
                // 所以：
                //
                // 坐标 + 权重只计算一次，
                // 然后遍历 channel。
                //
                // 比每个 channel 都重新算 floor/weight
                // 要合理很多。
                // --------------------------------------------
                for(std::uint32_t c = 0; c < channels; ++c)
                {
                    const float v00 = readFeatureOrZero(
                        feature,
                        n,
                        c,
                        y0,
                        x0,
                        channels,
                        input_height,
                        input_width
                    );
                    const float v01 = readFeatureOrZero(
                        feature,
                        n,
                        c,
                        y0,
                        x1,
                        channels,
                        input_height,
                        input_width
                    );
                    const float v10 = readFeatureOrZero(
                        feature,
                        n,
                        c,
                        y1,
                        x0,
                        channels,
                        input_height,
                        input_width
                    );
                    const float v11 = readFeatureOrZero(
                        feature,
                        n,
                        c,
                        y1,
                        x1,
                        channels,
                        input_height,
                        input_width
                    );
                    const float value =
                        v00 * w00 +
                        v01 * w01 +
                        v10 * w10 +
                        v11 * w11;
                    output[
                        outputIndex(
                            n,
                            c,
                            oy,
                            ox,
                            channels,
                            output_height,
                            output_width)
                    ] = value;
                }
            }
        }
    }
    return RKNN_SUCC;

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
        // custom_op.compute = gridSampleComputeProbe;
        custom_op.compute = gridSampleCompute;
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