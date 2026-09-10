#pragma once

#include <string>

#include "rkcam/ai/types.hpp"

namespace rkcam::ai {

/**
 * @brief 图片文件解码器。
 *
 * 职责：
 *   图片文件(JPEG/PNG/BMP...)
 *        ↓
 *   解码
 *        ↓
 *   RGB888 packed Image
 *
 * 输出 Image 的数据布局固定为：
 *
 *   RGBRGBRGBRGB...
 *
 * 即：
 *   - HWC
 *   - channels = 3
 *   - uint8_t
 *   - RGB 顺序
 *   - 每通道范围 [0, 255]
 *
 * 本类不负责：
 *   - resize
 *   - normalize
 *   - HWC -> CHW
 *   - RGB/IR concat
 *   - RKNN
 *   - 模型相关逻辑
 *
 * 模型需要的预处理由 RgbtPreprocessor 等模块负责。
 */

class ImageLoader final{
public:
    ImageLoader() = default;
    /**
     * @brief 从图片文件读取并统一转换为 RGB888。
     *
     * 即使输入图片本身是：
     *   - grayscale
     *   - RGBA
     *   - RGB
     *
     * 返回结果也统一为：
     *
     *   channels = 3
     *   RGB888
     *
     * @param path 图片文件路径
     *
     * @return 解码后的 RGB Image
     *
     * @throws std::invalid_argument
     *         path 为空
     *
     * @throws std::runtime_error
     *         图片读取/解码失败，或者得到非法尺寸
     */
    [[nodiscard]] static Image loadRgb(const std::string& path);
};
}
