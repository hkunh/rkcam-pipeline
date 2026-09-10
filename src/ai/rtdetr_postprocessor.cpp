#include "rkcam/ai/rtdetr_postprocessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rkcam::ai {

namespace {

/**
 * @brief Top-K 候选。
 *
 * flat_index 对应：
 *
 *   query_id * num_classes + class_id
 */
struct ScoreCandidate{
    float score = 0.0f;
    std::size_t flat_index = 0;
};


/**
 * @brief 数值稳定的 sigmoid。
 *
 * 普通：
 *
 *   1 / (1 + exp(-x))
 *
 * 当 x 很小时 exp(-x) 可能非常大，
 * 所以按正负分别计算。
 */
float sigmoid(float x) noexcept
{
    if(x >= 0.0f){
        const float z = std::exp(-x);
        return 1.0f / (1.0f + z);
    }
    const float z = std::exp(x);
    return z / (1.0f + z);
}
/**
 * @brief 验证 PreprocessInfo。
 */
void validatePreprocessInfo(
    const PreprocessInfo& info)
{
    if (info.original_width <= 0 ||
        info.original_height <= 0) {

        throw std::invalid_argument(
            "RtdetrPostprocessor: "
            "invalid original image size");
    }

    if (info.model_width <= 0 ||
        info.model_height <= 0) {

        throw std::invalid_argument(
            "RtdetrPostprocessor: "
            "invalid model input size");
    }

    if (info.scale_x <= 0.0f ||
        info.scale_y <= 0.0f) {

        throw std::invalid_argument(
            "RtdetrPostprocessor: "
            "invalid preprocess scale");
    }
}
/**
 * @brief 检查 float 是否正常。
 */
void validateFinite(
    float value,
    const char* name,
    std::size_t query_index
)
{
    if(!std::isfinite(value))
    {
        throw std::runtime_error(
            std::string("RtdetrPostprocessor: non-finte ")
            + name
            + "at query"
            + std::to_string(query_index)
        );
    }
}
}//namespace

RtdetrPostprocessor::RtdetrPostprocessor(RtdetrPostprocessConfig config) : config_(std::move(config))
{
    if(config_.score_threshold < 0.0f || config_.score_threshold > 1.0f)
    {
        throw std::invalid_argument("RtdetrPostprocessor: score_threshold must be in [0, 1]");
    }

}


std::vector<Detection> RtdetrPostprocessor::process(
    const FloatTensor& pred_logits,  //[B, num_queries, bboxes]
    const FloatTensor& pred_boxes,   //[B, num_queries, num_classes]
    const PreprocessInfo& preprocess_info
)const
{
    // ========================================================
    // 1. Validate tensors
    // ========================================================
    if(!pred_logits.valid())
    {
        throw std::invalid_argument("RtdetrPostprocessor: pred_logits tensor is invalid");
    }
    if(!pred_boxes.valid())
    {
        throw std::invalid_argument("RtdetrPostprocessor: pred_boxes tensor is invalid");
    }
    validatePreprocessInfo(preprocess_info);

    // --------------------------------------------------------
    // Expected:
    //
    // pred_logits:
    //   [1, Q, C]
    //
    // pred_boxes:
    //   [1, Q, 4]
    // --------------------------------------------------------
    if(pred_logits.shape.rank() != 3)
    {
        throw std::runtime_error("RtdetrPostprocessor: pred_boxes rank must be 3");
    }

    if(pred_boxes.shape.rank() != 3)
    {
        throw std::runtime_error("RtdetrPostprocessor: pred_boxes rank must be 3");
    }
    const auto& logits_dims = pred_logits.shape.dims;
    const auto& boxes_dims = pred_boxes.shape.dims;

    const auto& batch_logits = logits_dims[0];
    const auto& batch_boxes = boxes_dims[0];
    
    const auto& num_queries_logits = logits_dims[1];
    const auto& num_queries_boxes = boxes_dims[1];

    const auto& num_classes = logits_dims[2];
    const auto& box_values = boxes_dims[2];

    if(batch_logits != 1 || batch_boxes != 1)
    {
        throw std::runtime_error("RtdetrPostprocessor: only batch = 1 is currently supported");
    }
    if (num_queries_logits <= 0 ||
        num_queries_boxes <= 0 ||
        num_classes <= 0) {
        throw std::runtime_error(
            "RtdetrPostprocessor: "
            "invalid output dimensions");
    }
    if (num_queries_logits !=
        num_queries_boxes) {
        throw std::runtime_error(
            "RtdetrPostprocessor: "
            "pred_logits and pred_boxes "
            "query counts do not match");
    }
    if (box_values != 4) {

        throw std::runtime_error(
            "RtdetrPostprocessor: "
            "pred_boxes last dimension must be 4");
    }

    const std::size_t num_queries = static_cast<std::size_t>(num_queries_logits);
    const std::size_t classes = static_cast<std::size_t>(num_classes);

    // ========================================================
    // 2. sigmoid(logits)
    //
    // RT-DETR focal-loss path:
    //
    // scores shape:
    //
    //   [Q, C]
    //
    // flatten:
    //
    //   [Q * C]
    //
    // 当前模型：
    //      输出
    //   300 * 3 = 900
    // ========================================================
    const std::size_t score_count = num_queries * classes;
    std::vector<ScoreCandidate> candidates;
    candidates.reserve(score_count);
    for(std::size_t query = 0; query < num_queries; ++query)
    {
        for(std::size_t class_id = 0; class_id < classes; ++class_id)
        {

            const std::size_t index = query * classes + class_id;
            const float logit = pred_logits.data[index];
            validateFinite(logit, "logit", query);
            ScoreCandidate candidate;
            candidate.score = sigmoid(logit);
            candidate.flat_index = index;
            candidates.push_back(candidate);
        }
    }

    // ========================================================
    // 3. Global Top-K
    //
    // Python equivalent:
    //
    // scores = sigmoid(logits)
    //
    // scores, index = torch.topk(
    //     scores.flatten(1),
    //     num_top_queries,
    //     dim=-1
    // )
    //
    // labels =
    //     index % num_classes
    //
    // query =
    //     index / num_classes
    // ========================================================
    std::size_t top_k = config_.num_top_queries;
    // 0:
    // 自动使用 query 数。
    if(top_k == 0)
    {
        top_k = num_queries;
    }
    top_k = std::min(top_k, candidates.size());
    // 当前 900 个元素规模很小，
    // 直接完整排序代码更简单且稳定。
    std::sort(candidates.begin(), candidates.end(),
            [](const ScoreCandidate& lhs, const ScoreCandidate& rhs) //left hand side  //right hand side
            {
                return lhs.score > rhs.score;
            }
            );//高到低排
    

    // ========================================================
    // 4. Decode Top-K boxes
    // ========================================================
    std::vector<Detection> detections;
    detections.reserve(top_k);
    for(std::size_t i = 0; i < top_k; ++i)
    {
        const ScoreCandidate& candidate = candidates[i];
        // ----------------------------------------------------
        // score threshold
        //
        // candidates 已经按照 score 降序排列，
        // 所以后面所有结果一定更低。
        // ---------------------------------------------------
        if(candidate.score < config_.score_threshold)
        {
            break;
        }

        const std::size_t class_id = candidate.flat_index % classes;
        const std::size_t query_id = candidate.flat_index / classes;
        // ----------------------------------------------------
        // pred_boxes:
        //
        // [Q, 4]
        //
        // 每个 query：
        //
        //   cx, cy, w, h 这里的都是归一化到0~1的小数
        //
        // normalized
        // ----------------------------------------------------
        const std::size_t box_base = query_id * 4;
        const float cx = pred_boxes.data[box_base + 0];
        const float cy = pred_boxes.data[box_base + 1];
        const float w = pred_boxes.data[box_base + 2];
        const float h = pred_boxes.data[box_base + 3];

        validateFinite(cx, "bbox cx", query_id);
        validateFinite(cy, "bbox cy", query_id);
        validateFinite(w, "bbox width", query_id);
        validateFinite(h, "bbox height", query_id);

        // ====================================================
        // 5. normalized cxcywh
        //
        // →
        //
        // normalized xyxy
        // ====================================================

        const float x1_norm = cx - 0.5f * w;
        const float y1_norm = cy - 0.5f * h;
        const float x2_norm = cx + 0.5f * w;
        const float y2_norm = cy + 0.5f * h;

        // ====================================================
        // 6. normalized model coordinates
        //
        // →
        //
        // model pixel coordinates
        //
        // 例如：
        //
        // model = 640x640
        // ====================================================
        const float x1_model = x1_norm * static_cast<float>(preprocess_info.model_width);
        const float y1_model = y1_norm * static_cast<float>(preprocess_info.model_height);
        const float x2_model = x2_norm * static_cast<float>(preprocess_info.model_width);
        const float y2_model = y2_norm * static_cast<float>(preprocess_info.model_height);

        // ====================================================
        // 7. Undo preprocessing
        //
        // model coordinates
        //
        // →
        //
        // original image coordinates
        //
        //
        // 当前 stretch:
        //
        //   pad = 0
        //
        //   scale_x =
        //       model_w / original_w
        //
        //   scale_y =
        //       model_h / original_h
        //
        //
        // 以后如果使用 letterbox：
        //
        //   x_original =
        //       (x_model - pad_x) / scale
        //
        // 所以这一套公式不用重新设计。
        // ====================================================
        const float x1_original = 
            (x1_model - preprocess_info.pad_x) / preprocess_info.scale_x;
        const float y1_original = 
            (y1_model - preprocess_info.pad_y) / preprocess_info.scale_y;
        const float x2_original = 
            (x2_model - preprocess_info.pad_x) / preprocess_info.scale_x;
        const float y2_original =
            (y2_model - preprocess_info.pad_y) / preprocess_info.scale_y;

        // ====================================================
        // 8. Build Detection
        // ====================================================
        Detection detection;
        detection.class_id = static_cast<int>(class_id);
        detection.score = candidate.score;
        detection.x1 = x1_original;
        detection.y1 = y1_original;
        detection.x2 = x2_original;
        detection.y2 = y2_original;
        detections.push_back(detection); 

    }
    return detections;

}


} //namespace rkcam::ai