#include "rkcam/ai/unaligned_hit/hit_model_runner.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rkcam::ai::hit{
namespace{
constexpr std::size_t kStaticTemplateIndex = static_cast<std::size_t>(HiTModelInputSlot::StaticTemplate);

constexpr std::size_t kSearchIndex = static_cast<std::size_t>(HiTModelInputSlot::Search);

constexpr std::size_t kDynamicTemplateIndex = static_cast<std::size_t>(HiTModelInputSlot::DynamicTemplate);

constexpr std::size_t kTemplateGeometryIndex = static_cast<std::size_t>(HiTModelInputSlot::TemplateGeometry);

constexpr std::array<const char*, HiTModelInput::kInputCount> kExpectedInputNames
{
    "template",
    "search",
    "dynamic_template",
    "template_geometry",
};
// std::initializer_list<T> 是一个轻量级的只读容器，用于接收"用花括号 {...} 写的初始化列表"。它让函数能接受不定个数、同类型的参数
// dimsEqual(shape, {1, 6, 128, 128});   // 用花括号传入 4 个数
// dimsEqual(shape, {1, 3});             // 也可以传 2 个数
bool dimsEqual(
    const TensorShape& shape,
    std::initializer_list<std::int64_t> expected
)
{
    return shape.dims == std::vector<std::int64_t>(expected);
}

bool tensorIsCanonicalNchw(
    const FloatTensor& tensor,
    std::initializer_list<std::int64_t> expected_dims
)
{
    return tensor.valid()
    &&
    tensor.layout == TensorLayout::NCHW
    &&
    dimsEqual(tensor.shape, expected_dims);
}
/**
 * RKNN 编译后输入可能报告：
 *
 * NCHW:
 *   [1,6,128,128]
 *
 * 或 NHWC:
 *   [1,128,128,6]
 *
 * RknnEngine 本身支持根据 layout 做 semantic shape 检查，
 * 所以模型初始化阶段两种都允许。
 */
bool attrIsSemanticNchw(
    const TensorAttr& attr,
    std::int64_t n,
    std::int64_t c,
    std::int64_t h,
    std::int64_t w
) {
    if (
        attr.layout ==
        TensorLayout::NCHW
    ) {
        return dimsEqual(
            attr.shape,
            {n, c, h, w}
        );
    }

    if (
        attr.layout ==
        TensorLayout::NHWC
    ) {
        return dimsEqual(
            attr.shape,
            {n, h, w, c}
        );
    }

    return false;
}

std::string shapeToString(
    const TensorShape& shape
) {
    std::ostringstream oss;

    oss << "[";

    for (
        std::size_t i = 0;
        i < shape.dims.size();
        ++i
    ) {
        if (i != 0) {
            oss << ",";
        }

        oss << shape.dims[i];
    }

    oss << "]";

    return oss.str();
}

} //namespace


// ============================================================
// HiTModelInput
// ============================================================

HiTModelInput::HiTModelInput()
    : tensors(kInputCount) {
}


FloatTensor&
HiTModelInput::staticTemplate() noexcept {
    return tensors[
        kStaticTemplateIndex
    ];
}


const FloatTensor&
HiTModelInput::staticTemplate() const noexcept {
    return tensors[
        kStaticTemplateIndex
    ];
}


FloatTensor&
HiTModelInput::search() noexcept {
    return tensors[
        kSearchIndex
    ];
}


const FloatTensor&
HiTModelInput::search() const noexcept {
    return tensors[
        kSearchIndex
    ];
}


FloatTensor&
HiTModelInput::dynamicTemplate() noexcept {
    return tensors[
        kDynamicTemplateIndex
    ];
}


const FloatTensor&
HiTModelInput::dynamicTemplate() const noexcept {
    return tensors[
        kDynamicTemplateIndex
    ];
}


FloatTensor&
HiTModelInput::templateGeometry() noexcept {
    return tensors[
        kTemplateGeometryIndex
    ];
}


const FloatTensor&
HiTModelInput::templateGeometry() const noexcept {
    return tensors[
        kTemplateGeometryIndex
    ];
}


// ============================================================
// HiTModelRunner
// ============================================================

HiTModelRunner::HiTModelRunner(std::string model_path) : engine_(std::move(model_path), false)
{}

bool HiTModelRunner::init()
{
    last_error_.clear();
    if (!engine_.init()) {
        return fail(
            "failed to initialize RKNN engine: "
            + engine_.lastError()
        );
    }

    if (!validateModelInterface()) {
        engine_.close();
        return false;
    }
    return true;
}


void HiTModelRunner::close() noexcept {
    engine_.close();

    last_error_.clear();
}


bool HiTModelRunner::initialized() const noexcept {
    return engine_.initialized();
}


const std::string&
HiTModelRunner::modelPath() const noexcept {
    return engine_.modelPath();
}


const std::string&
HiTModelRunner::lastError() const noexcept {
    return last_error_;
}


const RknnEngine&
HiTModelRunner::engine() const noexcept {
    return engine_;
}


// ============================================================
// Model interface validation
// ============================================================
bool HiTModelRunner::validateModelInterface() {

    // --------------------------------------------------------
    // 1. Input/output count
    // --------------------------------------------------------

    if (
        engine_.inputCount()
        !=
        HiTModelInput::kInputCount
    ) {
        std::ostringstream oss;

        oss
            << "HiT RKNN input count mismatch: got "
            << engine_.inputCount()
            << ", expected "
            << HiTModelInput::kInputCount;

        return fail(
            oss.str()
        );
    }


    if (
        engine_.outputCount() != 1
    ) {
        std::ostringstream oss;

        oss
            << "HiT RKNN output count mismatch: got "
            << engine_.outputCount()
            << ", expected 1";

        return fail(
            oss.str()
        );
    }


    // --------------------------------------------------------
    // 2. Input names
    //
    // 这里故意做严格检查。
    //
    // 原因：
    // 输入全部是 float tensor，
    // 如果 template/search 顺序错了，
    // RKNN Runtime 本身未必能够发现。
    // --------------------------------------------------------

    for (
        std::size_t i = 0;
        i < HiTModelInput::kInputCount;
        ++i
    ) {
        const TensorAttr& attr =
            engine_.inputAttr(i);

        if (
            attr.name
            !=
            kExpectedInputNames[i]
        ) {
            std::ostringstream oss;

            oss
                << "HiT RKNN input["
                << i
                << "] name mismatch: got '"
                << attr.name
                << "', expected '"
                << kExpectedInputNames[i]
                << "'";

            return fail(
                oss.str()
            );
        }
    }


    // --------------------------------------------------------
    // 3. template
    // --------------------------------------------------------

    const TensorAttr& static_attr =
        engine_.inputAttr(
            kStaticTemplateIndex
        );

    if (
        !attrIsSemanticNchw(
            static_attr,
            1,
            6,
            128,
            128
        )
    ) {
        return fail(
            "template input shape/layout mismatch: got "
            + shapeToString(
                static_attr.shape
            )
        );
    }


    // --------------------------------------------------------
    // 4. search
    // --------------------------------------------------------

    const TensorAttr& search_attr =
        engine_.inputAttr(
            kSearchIndex
        );

    if (
        !attrIsSemanticNchw(
            search_attr,
            1,
            6,
            256,
            256
        )
    ) {
        return fail(
            "search input shape/layout mismatch: got "
            + shapeToString(
                search_attr.shape
            )
        );
    }


    // --------------------------------------------------------
    // 5. dynamic_template
    // --------------------------------------------------------

    const TensorAttr& dynamic_attr =
        engine_.inputAttr(
            kDynamicTemplateIndex
        );

    if (
        !attrIsSemanticNchw(
            dynamic_attr,
            1,
            6,
            128,
            128
        )
    ) {
        return fail(
            "dynamic_template input shape/layout mismatch: got "
            + shapeToString(
                dynamic_attr.shape
            )
        );
    }


    // --------------------------------------------------------
    // 6. geometry
    // --------------------------------------------------------

    const TensorAttr& geometry_attr =
        engine_.inputAttr(
            kTemplateGeometryIndex
        );

    if (
        !dimsEqual(
            geometry_attr.shape,
            {1, 4}
        )
    ) {
        return fail(
            "template_geometry input shape mismatch: got "
            + shapeToString(
                geometry_attr.shape
            )
            + ", expected [1,4]"
        );
    }


    // --------------------------------------------------------
    // 7. output
    // --------------------------------------------------------

    const TensorAttr& output_attr =
        engine_.outputAttr(0);

    if (
        output_attr.name
        !=
        "pred_boxes"
    ) {
        return fail(
            "HiT RKNN output name mismatch: got '"
            + output_attr.name
            + "', expected 'pred_boxes'"
        );
    }


    if (
        output_attr.element_count
        !=
        4
    ) {
        std::ostringstream oss;

        oss
            << "pred_boxes element count mismatch: got "
            << output_attr.element_count
            << ", expected 4";

        return fail(
            oss.str()
        );
    }


    return true;
}

// ============================================================
// Runtime input validation
// ============================================================

bool HiTModelRunner::validateInput(
    const HiTModelInput& input
) {

    if (
        input.tensors.size()
        !=
        HiTModelInput::kInputCount
    ) {
        std::ostringstream oss;

        oss
            << "HiTModelInput tensor count mismatch: got "
            << input.tensors.size()
            << ", expected "
            << HiTModelInput::kInputCount;

        return fail(
            oss.str()
        );
    }


    // --------------------------------------------------------
    // template
    // --------------------------------------------------------

    if (
        !tensorIsCanonicalNchw(
            input.staticTemplate(),
            {1, 6, 128, 128}
        )
    ) {
        return fail(
            "static template must be valid NCHW "
            "[1,6,128,128]"
        );
    }


    // --------------------------------------------------------
    // search
    // --------------------------------------------------------

    if (
        !tensorIsCanonicalNchw(
            input.search(),
            {1, 6, 256, 256}
        )
    ) {
        return fail(
            "search must be valid NCHW "
            "[1,6,256,256]"
        );
    }


    // --------------------------------------------------------
    // dynamic template
    // --------------------------------------------------------

    if (
        !tensorIsCanonicalNchw(
            input.dynamicTemplate(),
            {1, 6, 128, 128}
        )
    ) {
        return fail(
            "dynamic template must be valid NCHW "
            "[1,6,128,128]"
        );
    }


    // --------------------------------------------------------
    // template geometry
    // --------------------------------------------------------

    const FloatTensor& geometry =
        input.templateGeometry();


    if (
        !geometry.valid()
        ||
        !dimsEqual(
            geometry.shape,
            {1, 4}
        )
    ) {
        return fail(
            "template_geometry must be a valid "
            "[1,4] float tensor"
        );
    }


    /**
     * template_geometry 是 rank=2，
     * 不存在真正意义上的 NCHW/NHWC。
     *
     * 但是现有 RknnEngine::infer() 最终仍需要一个
     * TensorLayout 转换成 rknn_tensor_format。
     *
     * 所以这里要求它使用 RKNN 模型 metadata 报告的 layout。
     */
    if (
        geometry.layout
        !=
        engine_.inputAttr(
            kTemplateGeometryIndex
        ).layout
    ) {
        return fail(
            "template_geometry layout must match "
            "RKNN inputAttr(3).layout"
        );
    }


    return true;
}

bool HiTModelRunner::infer(const HiTModelInput& input, HiTRawOutput& output)
{
    last_error_.clear();
    output = HiTRawOutput{};

    if (
        !engine_.initialized()
    ) {
        return fail(
            "HiTModelRunner is not initialized"
        );
    }
    if (
        !validateInput(input)
    ) {
        return false;
    }

    std::vector<FloatTensor> outputs;
    /**
     * 这里直接复用已有多输入接口。
     *
     * 不写：
     *
     * rknn_inputs_set()
     * rknn_run()
     * rknn_outputs_get()
     *
     * 这些全部由 RknnEngine 负责。
     */
    if(!engine_.infer(input.tensors, outputs))
    {
        return fail(
            "RKNN inference failed: "
            + engine_.lastError()
        );
    }

    if (
        outputs.size()
        !=
        1
    ) {
        std::ostringstream oss;

        oss
            << "RKNN returned "
            << outputs.size()
            << " outputs, expected 1";

        return fail(
            oss.str()
        );
    }

    const FloatTensor& pred = outputs[0];

    if (
        !pred.valid()
    ) {
        return fail(
            "RKNN pred_boxes output is invalid"
        );
    }


    if (
        pred.data.size()
        !=
        4
    ) {
        std::ostringstream oss;

        oss
            << "RKNN pred_boxes data size mismatch: got "
            << pred.data.size()
            << ", expected 4";

        return fail(
            oss.str()
        );
    }


    for (
        std::size_t i = 0;
        i < output.pred_boxes.size();
        ++i
    ) {
        output.pred_boxes[i] =
            pred.data[i];
    }


    return true;

}

// ============================================================
// Error
// ============================================================

bool HiTModelRunner::fail(
    std::string message
) {
    last_error_ =
        std::move(message);

    return false;
}

}