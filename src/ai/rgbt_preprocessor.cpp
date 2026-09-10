#include "rkcam/ai/rgbt_preprocessor.hpp"

#include <algorithm> //std::min
#include <cmath>
#include <cstddef> //std::size_t
#include <cstdint> //std::uint8_t
#include <stdexcept>
#include <string>
#include <utility>

namespace rkcam::ai
{
namespace{
constexpr int kRgbChannels = 3; //constexpr 编译时常量
constexpr int kRgbtChannels = 6;

/**
 * @brief 从模型 TensorAttr 中提取标准 N/C/H/W。
 *
 * TensorShape 本身保持通用 dims，
 * 这里根据 layout 对 dims 做解释。
 */

struct ModelInputShape{
    int n = 0;
    int c = 0;
    int h = 0;
    int w = 0;
};

ModelInputShape parseModelInputShape(const TensorAttr& attr)
{
    if(attr.shape.rank() != 4)
    {
        throw std::runtime_error("RgbtPreprocessor: model input rank must be 4");
    }
    const auto & dims = attr.shape.dims;
    ModelInputShape shape;
    switch (attr.layout) {

    case TensorLayout::NCHW:
        shape.n = static_cast<int>(dims[0]);
        shape.c = static_cast<int>(dims[1]);
        shape.h = static_cast<int>(dims[2]);
        shape.w = static_cast<int>(dims[3]);
        break;

    case TensorLayout::NHWC:
        shape.n = static_cast<int>(dims[0]);
        shape.h = static_cast<int>(dims[1]);
        shape.w = static_cast<int>(dims[2]);
        shape.c = static_cast<int>(dims[3]);
        break;

    default:
        throw std::runtime_error(
            "RgbtPreprocessor: unsupported model input layout");
    }

    if (shape.n != 1) {
        throw std::runtime_error(
            "RgbtPreprocessor: only batch=1 is currently supported");
    }

    if (shape.c != kRgbtChannels) {
        throw std::runtime_error(
            "RgbtPreprocessor: model input channels must be 6, got "
            + std::to_string(shape.c));
    }

    if (shape.h <= 0 || shape.w <= 0) {
        throw std::runtime_error(
            "RgbtPreprocessor: invalid model input size");
    }

    return shape;
}
/**
 * @brief 检查 Image 是否符合预处理器要求。
 */

void validateRgbImage(
    const Image& image,
    const char* name
)
{
    if(!image.valid())
    {
        throw std::invalid_argument(
            std::string("RgbtPreprocessor: invalid")
            + name
            + " image"
        );
    }

    if(image.channels != kRgbChannels)
    {
        throw std::invalid_argument(
            std::string("RgbtPreprocessor: ")
            + name
            + " image must be RGB888, channels=3"
        );
    }
}

/**
 * @brief float → uint8，带边界保护。
 */
std::uint8_t toUint8(float value)
{
    value = std::clamp(value, 0.0f, 255.0f);
    return static_cast<std::uint8_t>(std::lround(value)); //std::lround 四舍五入,将中间值舍入到绝对值更大的方向
}

}//namespace

Image RgbtPreprocessor::resizeBilinearRgb(
    const Image& src,
    int dst_width,
    int dst_height
)
{
    validateRgbImage(src, "source");
    if(dst_width <= 0 || dst_height <= 0)
    {
        throw std::invalid_argument(
            "RgbtPreprocessor::resizeBilinearRgb: "
            "invalid destination size"
        );
    }
    // --------------------------------------------------------
    // 如果尺寸完全一致，直接复制。
    //
    // 这里仍然返回独立 Image，
    // 保证接口行为一致。
    // --------------------------------------------------------
    if(src.width == dst_width && src.height == dst_height)
    {
        return src;
    }

    Image dst;
    dst.width = dst_width;
    dst.height = dst_height;
    dst.channels = kRgbChannels;
    const std::size_t dst_bytes = 
        static_cast<std::size_t>(dst_width) *
        static_cast<std::size_t>(dst_height) *
        static_cast<std::size_t>(kRgbChannels);
    dst.data.resize(dst_bytes);

    // --------------------------------------------------------
    // Half-pixel bilinear mapping
    //
    // dst pixel center:
    //
    //   (x + 0.5)
    //
    // 映射到 source：
    //
    //   src_x =
    //     (dst_x + 0.5) * src_width / dst_width
    //     - 0.5
    //
    // 这种映射方式是常见的图像 resize 方式，
    // 对当前 640x512 -> 640x640 场景适用。双线性插值
    // --------------------------------------------------------
    const float scale_x = 
        static_cast<float>(src.width) /
        static_cast<float>(dst_width) ;
    const float scale_y = 
        static_cast<float>(src.height) /
        static_cast<float>(dst_height);
    
    for(int dy = 0; dy < dst_height; ++dy)
    {
        float src_y = (static_cast<float>(dy) + 0.5f) * scale_y - 0.5;
        src_y = std::clamp(src_y, 0.0f, static_cast<float>(src.height - 1));

        const int y0 = static_cast<int>(std::floor(src_y));
        const int y1 = std::min(y0 + 1, src.height - 1);

        const float wy = src_y - static_cast<float>(y0);

        for(int dx = 0; dx < dst_width; ++dx)
        {
            float src_x = (static_cast<float>(dx) + 0.5f) * scale_x - 0.5f;
            src_x = std::clamp(src_x, 0.0f, static_cast<float>(src.width - 1));

            const int x0 = static_cast<int>(std::floor(src_x));
            const int x1 = std::min(x0 + 1, src.width - 1);
            const float wx = src_x - static_cast<float>(x0);

            const std::size_t src00 = 
                (
                    static_cast<std::size_t>(y0) *
                    static_cast<std::size_t>(src.width) +
                    static_cast<std::size_t>(x0)  //二维坐标转一维坐标
                ) *
                kRgbChannels;

            const std::size_t src01 = 
                (
                    static_cast<std::size_t>(y0) *
                    static_cast<std::size_t>(src.width) +
                    static_cast<std::size_t>(x1)
                ) *
                kRgbChannels;

            const std::size_t src10 = 
                (
                    static_cast<std::size_t>(y1) *
                    static_cast<std::size_t>(src.width) +
                    static_cast<std::size_t>(x0)
                ) *
                kRgbChannels;
            
            const std::size_t src11 = 
                (
                    static_cast<std::size_t>(y1) *
                    static_cast<std::size_t>(src.width) +
                    static_cast<std::size_t>(x1)
                ) *
                kRgbChannels;
            
            const std::size_t dst_index = 
                (
                    static_cast<std::size_t>(dy) *
                    static_cast<std::size_t>(dst_width) +
                    static_cast<std::size_t>(dx)
                ) *
                kRgbChannels;
            
            for(int c = 0; c < kRgbChannels; ++c)
            {
                const float p00 = static_cast<float>(src.data[src00 + c]);
                const float p01 = static_cast<float>(src.data[src01 + c]);
                const float p10 = static_cast<float>(src.data[src10 + c]);
                const float p11 = static_cast<float>(src.data[src11 + c]);

                // --------------------------------------------
                // horizontal interpolation
                // --------------------------------------------
                const float top = p00 + (p01 - p00) * wx;
                const float bottom = p10 + (p11 - p10) * wx;
                // --------------------------------------------
                // vertical interpolation
                // --------------------------------------------
                const float value = top + (bottom - top) * wy;
                dst.data[dst_index + c] = toUint8(value);
            } 
        }
    }
    if (!dst.valid()) {
        throw std::runtime_error(
            "RgbtPreprocessor::resizeBilinearRgb: "
            "generated invalid image");
    }

    return dst;

}



PreprocessResult RgbtPreprocessor::process(
    const Image& rgb,
    const Image& ir,
    const TensorAttr& model_input
)const
{
    // --------------------------------------------------------
    // 1. Validate input Image
    // --------------------------------------------------------
    validateRgbImage(rgb, "RGB");
    validateRgbImage(ir, "IR");

    // --------------------------------------------------------
    // 2. Parse model input shape
    // --------------------------------------------------------
    const ModelInputShape model_shape = parseModelInputShape(model_input);
    const int model_width = model_shape.w;
    const int model_height = model_shape.h;

    // --------------------------------------------------------
    // 3. Resize RGB / IR
    //
    // 当前模型训练 / ONNX validation 使用：
    //
    // direct stretch
    //
    // 不做 letterbox。
    // --------------------------------------------------------
    Image rgb_resized = resizeBilinearRgb(rgb, model_width, model_height);
    Image ir_resized =  resizeBilinearRgb(ir, model_width, model_height);
    // --------------------------------------------------------
    // 4. Prepare output Tensor
    // --------------------------------------------------------
    PreprocessResult result;
    result.tensor.shape = model_input.shape;
    result.tensor.layout = model_input.layout;
    const std::size_t element_count = model_input.shape.elementCount();
    if (element_count == 0) {
        throw std::runtime_error(
            "RgbtPreprocessor: "
            "model input has invalid element count");
    }
    result.tensor.data.resize(element_count);
    constexpr float kInv255 = 1.0f / 255.0f;
    const std::size_t pixels =
        static_cast<std::size_t>(model_width) *
        static_cast<std::size_t>(model_height);

    // --------------------------------------------------------
    // 5. HWC RGB888
    //
    // →
    //
    // model tensor
    //
    // 当前 NCHW:
    //
    // channel 0 = RGB R
    // channel 1 = RGB G
    // channel 2 = RGB B
    // channel 3 = IR  R
    // channel 4 = IR  G
    // channel 5 = IR  B
    // --------------------------------------------------------
    if(model_input.layout == TensorLayout::NCHW) //N是批次
    {
        for(std::size_t i = 0; i < pixels; ++i)
        {
            const std::size_t src = i * kRgbChannels;
            result.tensor.data[0 * pixels + i] = static_cast<float>(rgb_resized.data[src + 0]) * kInv255;
            result.tensor.data[1 * pixels + i] = static_cast<float>(rgb_resized.data[src + 1]) * kInv255;
            result.tensor.data[2 * pixels + i] = static_cast<float>(rgb_resized.data[src + 2]) * kInv255;
            // IR
            result.tensor.data[3 * pixels + i] = static_cast<float>(ir_resized.data[src + 0]) * kInv255;
            result.tensor.data[4 * pixels + i] = static_cast<float>(ir_resized.data[src + 1]) * kInv255;
            result.tensor.data[5 * pixels + i] = static_cast<float>(ir_resized.data[src + 2]) * kInv255;
        }
    }
    else if(model_input.layout == TensorLayout::NHWC)
    {
        for(std::size_t i = 0; i < pixels; ++i)
        {
            const std::size_t src = i * kRgbChannels;
            const std::size_t dst = i * kRgbtChannels;
            //RGB
            result.tensor.data[dst + 0] = static_cast<float>(rgb_resized.data[src + 0]) * kInv255;
            result.tensor.data[dst + 1] = static_cast<float>(rgb_resized.data[src + 1]) * kInv255;
            result.tensor.data[dst + 2] = static_cast<float>(rgb_resized.data[src + 2]) * kInv255;
            //IR
            result.tensor.data[dst + 3] = static_cast<float>(ir_resized.data[src + 0]) * kInv255;
            result.tensor.data[dst + 4] = static_cast<float>(ir_resized.data[src + 1]) * kInv255;
            result.tensor.data[dst + 5] = static_cast<float>(ir_resized.data[src + 2]) * kInv255;
        }
    }
    else{
        throw std::runtime_error("RgbPreprocessor: unsupported tensor layout");
    }

    // --------------------------------------------------------
    // 6. Save geometry information
    //
    // 和你原来的 Python 一样：
    // 最终 bbox 坐标以 visible/RGB 原图为基准。
    // --------------------------------------------------------
    result.info.original_width = rgb.width;
    result.info.original_height = rgb.height;
    result.info.model_width = model_width;
    result.info.model_height = model_height;

    // 当前使用 stretch resize
    //
    // 640x512 -> 640x640:
    //
    // scale_x = 1.0
    // scale_y = 1.25
    //
    // 没有 padding。
    result.info.scale_x = 
        static_cast<float>(model_width) /
        static_cast<float>(rgb.width);
    result.info.scale_y = 
        static_cast<float>(model_height) /
        static_cast<float>(rgb.height);
    result.info.pad_x = 0.0f;
    result.info.pad_y = 0.0f;

    // --------------------------------------------------------
    // 7. Final validation
    // --------------------------------------------------------

    if (!result.tensor.valid()) {
        throw std::runtime_error(
            "RgbtPreprocessor: generated tensor is invalid");
    }

    return result;

}

}//namespace rkcam::ai