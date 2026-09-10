#pragma once

#include "rkcam/ai/types.hpp"

namespace rkcam::ai{

/**
 * @brief RGBT 模型输入预处理结果。
 *
 * tensor:
 *   最终可以直接送入推理引擎的 FloatTensor。
 *
 * info:
 *   保存本次预处理的几何信息，
 *   后续 PostProcessor 用它把检测框恢复到原图坐标。
 */


struct PreprocessResult
{
    FloatTensor tensor;
    PreprocessInfo info;  
};


/**
 * @brief RGB + Thermal 双模态输入预处理器。
 *
 * 当前预处理规则与现有 RGBT RT-DETR 模型保持一致：
 *
 * RGB Image:
 *   RGB888 uint8 HWC
 *
 * Thermal Image:
 *   RGB888 uint8 HWC
 *
 *        ↓
 *
 * 根据 model_input 自动读取目标 H / W
 *
 *        ↓
 *
 * Bilinear resize
 * 直接拉伸到模型输入尺寸
 *
 *        ↓
 *
 * uint8 [0,255]
 * →
 * float32 [0,1]
 *
 *        ↓
 *
 * RGB + IR channel concat
 *
 * NCHW:
 *   [R, G, B, IR-R, IR-G, IR-B]
 *
 * 输出：
 *   [1, 6, H, W]
 *
 *
 * 本类不负责：
 *   - 图片文件读取
 *   - JPEG / PNG 解码
 *   - RKNN Runtime
 *   - 推理
 *   - sigmoid
 *   - bbox 后处理
 *   - 画框
 */
class RgbtPreprocessor final{ //final 禁止继承或重写
public:
    RgbtPreprocessor() = default;
    /**
     * @brief 将 RGB / IR Image 转换成模型输入 Tensor。
     *
     * @param rgb
     *        RGB 图像。
     *        必须是 RGB888:
     *          channels = 3
     *          uint8
     *          HWC packed
     *
     * @param ir
     *        Thermal 图像。
     *        同样必须已经转换为 RGB888。
     *
     * @param model_input
     *        从 RknnEngine::inputAttr() 获取的模型输入属性。
     *
     * 当前支持：
     *   - rank = 4
     *   - batch = 1
     *   - channels = 6
     *   - NCHW
     *   - NHWC
     *
     * @return
     *        FloatTensor + PreprocessInfo
     *
     * @throws std::invalid_argument
     *        输入 Image 非法
     *
     * @throws std::runtime_error
     *        模型 shape / layout 不支持
     */
    [[nodiscard]] PreprocessResult process(
        const Image& rgb,
        const Image& ir,
        const TensorAttr& model_input
    ) const;

private:
    /**
     * @brief RGB888 Bilinear resize。
     *
     * src:
     *   RGBRGBRGB...
     *
     * dst:
     *   RGBRGBRGB...
     *
     * 输出仍然是 uint8 RGB888。
     */
    [[nodiscard]]
    static Image resizeBilinearRgb(
        const Image& src,
        int dst_width,
        int dst_height
    );

};

}//namespace rkcam::ai