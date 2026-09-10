#pragma once

#include <cstddef>
#include <vector>

#include "rkcam/ai/types.hpp"

namespace rkcam::ai {

/**
 * @brief RT-DETR 后处理配置。
 *
 * 当前实现对应 focal-loss RT-DETR：
 *
 *   pred_logits
 *      ↓
 *   sigmoid
 *      ↓
 *   flatten(query × class)
 *      ↓
 *   global Top-K
 *
 * num_top_queries:
 *   0 表示自动使用模型 query 数。
 *
 *   例如当前：
 *     pred_logits = [1, 300, 3]
 *
 *   则：
 *     num_queries = 300
 *     num_top_queries = 300
 *
 * score_threshold:
 *   对 Top-K 结果进一步做置信度过滤。
 *
 *   默认 0.0，表示不额外过滤，
 *   这样更接近原 RT-DETR PostProcessor 的行为。
 */

struct RtdetrPostprocessConfig{
    std::size_t num_top_queries = 0;
    float score_threshold = 0.0f;
};


/**
 * @brief RT-DETR raw output 后处理器。
 *
 * 输入：
 *
 *   pred_logits:
 *     [1, num_queries, num_classes]
 *
 *   pred_boxes:
 *     [1, num_queries, 4]
 *
 * bbox 格式：
 *
 *   [cx, cy, w, h]
 *
 * 并且为归一化坐标。
 *
 *
 * 当前处理流程：
 *
 *   pred_logits
 *       ↓
 *   sigmoid
 *       ↓
 *   flatten(query × class)
 *       ↓
 *   Top-K
 *       ↓
 *   query_id + class_id
 *
 *   pred_boxes[query_id]
 *       ↓
 *   normalized cxcywh
 *       ↓
 *   normalized xyxy
 *       ↓
 *   model input pixel coordinates
 *       ↓
 *   根据 PreprocessInfo
 *   映射回 original image
 *       ↓
 *   Detection
 *
 *
 * 本类不负责：
 *
 *   - RKNN Runtime
 *   - 图片读取
 *   - resize
 *   - RGB / IR preprocessing
 *   - 绘制 bbox
 *   - NMS
 *
 * RT-DETR 本身不依赖传统 NMS。
 */
class RtdetrPostprocessor final{
public:
    explicit RtdetrPostprocessor(RtdetrPostprocessConfig config = {});


    /**
     * @brief 执行 RT-DETR 后处理。
     *
     * @param pred_logits
     *        [1, Q, C] float32
     *
     * @param pred_boxes
     *        [1, Q, 4] float32
     *        normalized cxcywh
     *
     * @param preprocess_info
     *        当前图片预处理时记录的：
     *
     *          original size
     *          model size
     *          scale
     *          padding
     *
     * @return
     *        按 score 从高到低排列的 Detection。
     *
     * @throws std::invalid_argument
     *         Tensor / PreprocessInfo 非法
     *
     * @throws std::runtime_error
     *         Tensor shape 不符合 RT-DETR 输出格式
     */
    [[nodiscard]] std::vector<Detection> process(
        const FloatTensor& pred_logits,
        const FloatTensor& pred_boxes,
        const PreprocessInfo& preprocess_info
    ) const;
    
    [[nodiscard]] const RtdetrPostprocessConfig& config() const noexcept
    {
        return config_;
    }
private:
    RtdetrPostprocessConfig config_;
};

}//namespace rkcam::ai
