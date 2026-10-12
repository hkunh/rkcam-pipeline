#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "rkcam/ai/rknn_engine.hpp"
#include "rkcam/ai/types.hpp"

namespace rkcam::ai::hit{
enum class HiTModelInputSlot: std::size_t{
    StaticTemplate = 0,
    Search = 1,
    DynamicTemplate = 2,
    TemplateGeometry = 3,
};
/**
 * @brief HiT 模型的 4 个已经准备好的输入 tensor。
 *
 * 固定顺序：
 *
 * 0 template
 *   [1, 6, 128, 128]
 *
 * 1 search
 *   [1, 6, 256, 256]
 *
 * 2 dynamic_template
 *   [1, 6, 128, 128]
 *
 * 3 template_geometry
 *   [1, 4]
 *
 * tensors 使用 vector 是为了可以直接传给：
 *
 *   RknnEngine::infer(inputs, outputs)
 *
 * 避免 infer 时重新复制四个大 tensor。
 */
struct HiTModelInput {
    static constexpr std::size_t kInputCount = 4;
    HiTModelInput();
    [[nodiscard]] FloatTensor& staticTemplate() noexcept;
    [[nodiscard]] const FloatTensor& staticTemplate() const noexcept;
    [[nodiscard]] FloatTensor& search() noexcept;
    [[nodiscard]] const FloatTensor& search() const noexcept;
    [[nodiscard]] FloatTensor& dynamicTemplate() noexcept;
    [[nodiscard]] const FloatTensor& dynamicTemplate() const noexcept;
    [[nodiscard]] FloatTensor& templateGeometry() noexcept;
    [[nodiscard]] const FloatTensor& templateGeometry() const noexcept;
    std::vector<FloatTensor> tensors;
};

/**
 * @brief HiT 模型裸输出。
 *
 * pred_boxes:
 *
 *   [cx, cy, w, h]
 *
 * 语义：
 *   - normalized
 *   - search crop 坐标系
 *
 * 这里完全不做原图坐标恢复。
 */

struct HiTRawOutput{
    std::array<float, 4> pred_boxes{
        0.0f,
        0.0f,
        0.0f,
        0.0f,
    };
};


/**
 * @brief HiT RKNN 模型运行器。
 *
 * 职责：
 *
 * 4 个已经准备好的 FloatTensor
 *              ↓
 *          RknnEngine
 *              ↓
 *      pred_boxes [1,1,4]
 *
 * 不负责：
 *
 * - crop
 * - resize
 * - normalize
 * - H0
 * - bbox 坐标恢复
 * - dynamic template 更新策略
 * - tracker state
 * - 第 20 帧更新
 */
class HiTModelRunner final{
public:
    explicit HiTModelRunner(std:: string model_path);
    HiTModelRunner(const HiTModelRunner&) = delete;
    HiTModelRunner& operator=(const HiTModelRunner&) = delete;

    HiTModelRunner(HiTModelRunner&&) noexcept = default;
    HiTModelRunner& operator=(HiTModelRunner&& ) noexcept = default;

    /**
     * @brief 初始化 RKNN，并验证模型接口。
     */
    bool init();
    void close() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] const std::string& modelPath() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;

    /**
     * @brief 运行一次裸 HiT forward。
     */
    bool infer(const HiTModelInput& input, HiTRawOutput& output);
    /**
     * @brief 暴露模型 metadata，方便 standalone test。
     *
     * 例如：
     *
     * runner.engine().inputAttr(3)
     *
     * 可以查看 template_geometry 的实际 RKNN layout。
     */
    [[nodiscard]] const RknnEngine& engine() const noexcept;
private:
    bool validateModelInterface();
    bool validateInput(const HiTModelInput& input);
    bool fail(std::string message);
    RknnEngine engine_;
    std::string last_error_;
};



}//namespace rkcam::ai::hit