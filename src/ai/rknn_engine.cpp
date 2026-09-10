#include "rkcam/ai/rknn_engine.hpp"
#include "rkcam/platform/rockchip/rknn/grid_sample_custom_op.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "rknn_api.h"

namespace rkcam::ai{
namespace{

TensorLayout fromRknnLayout(rknn_tensor_format fmt) noexcept{
    switch(fmt){
        case RKNN_TENSOR_NCHW:
            return TensorLayout::NCHW;
        case RKNN_TENSOR_NHWC:
            return TensorLayout::NHWC;
        case RKNN_TENSOR_NC1HWC2:
            return TensorLayout::NC1HWC2;
        default:
            return TensorLayout::Unknown;
    }
}
bool toRknnLayout(
    TensorLayout layout,
    rknn_tensor_format& fmt
) noexcept{
    switch(layout){
        case TensorLayout::NCHW:
            fmt = RKNN_TENSOR_NCHW;
            return true;
        case TensorLayout::NHWC:
            fmt = RKNN_TENSOR_NHWC;
            return true;
        case TensorLayout::NC1HWC2:
            fmt = RKNN_TENSOR_NC1HWC2;
            return true;
        default:
           return false; 
    }
}

TensorDataType fromRknnDataType(rknn_tensor_type type) noexcept {
    switch (type) {
        case RKNN_TENSOR_FLOAT32:
            return TensorDataType::Float32;
        case RKNN_TENSOR_FLOAT16:
            return TensorDataType::Float16;
        case RKNN_TENSOR_INT8:
            return TensorDataType::Int8;
        case RKNN_TENSOR_UINT8:
            return TensorDataType::UInt8;
        case RKNN_TENSOR_INT16:
            return TensorDataType::Int16;
        case RKNN_TENSOR_UINT16:
            return TensorDataType::UInt16;
        case RKNN_TENSOR_INT32:
            return TensorDataType::Int32;
        case RKNN_TENSOR_UINT32:
            return TensorDataType::UInt32;
        case RKNN_TENSOR_INT64:
            return TensorDataType::Int64;
        case RKNN_TENSOR_BOOL:
            return TensorDataType::Bool;
        default:
            return TensorDataType::Unknown;
    }
}

TensorQuantizationType fromRknnQuantizationType(
    rknn_tensor_qnt_type type
) noexcept {
    switch (type) {
        case RKNN_TENSOR_QNT_NONE:
            return TensorQuantizationType::None;
        case RKNN_TENSOR_QNT_DFP:
            return TensorQuantizationType::Dfp;
        case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
            return TensorQuantizationType::AffineAsymmetric;
        default:
            return TensorQuantizationType::Unknown;
    }
}

TensorAttr convertAttr(const rknn_tensor_attr& attr)
{
    TensorAttr result;
    result.index = attr.index;
    result.name = attr.name;
    result.layout = fromRknnLayout(attr.fmt);
    result.data_type = fromRknnDataType(attr.type);
    result.quantization_type = fromRknnQuantizationType(attr.qnt_type);
    result.element_count = attr.n_elems;
    result.byte_size = attr.size;
    result.byte_size_with_stride = attr.size_with_stride;
    result.zero_point = attr.zp;
    result.scale = attr.scale;

    result.shape.dims.reserve(attr.n_dims);
    for(std::uint32_t i = 0; i < attr.n_dims; ++i)
    {
        result.shape.dims.push_back(static_cast<int64_t>(attr.dims[i]));
    }
    return result;
}

bool isFileReadable(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

std::string makeRknnError(const char* operation, int ret)
{
    std::ostringstream oss;
    oss << operation << "failed, ret=" << ret;
    return oss.str();
}

bool getSemanticNchw(
    const TensorShape& shape, 
    TensorLayout layout,
    std::int64_t& n,
    std::int64_t& c,
    std::int64_t& h,
    std::int64_t& w
)   noexcept{
    
    if(shape.dims.size() != 4)
    {
        return false;
    }
    if(layout == TensorLayout::NCHW)
    {
        n = shape.dims[0];
        c = shape.dims[1];
        h = shape.dims[2];
        w = shape.dims[3];
        return true;
    }
    if(layout == TensorLayout::NHWC)
    {
        n = shape.dims[0];
        h = shape.dims[1];
        w = shape.dims[2];
        c = shape.dims[3];
        return true;
    }
    return false;
}

bool shapesCompatible(
    const FloatTensor& input,
    const TensorAttr& expected
) noexcept
{
    if(input.shape.dims == expected.shape.dims && input.layout == expected.layout)
    {
        return true;
    }

    std::int64_t in_n = 0;
    std::int64_t in_c = 0;
    std::int64_t in_h = 0;
    std::int64_t in_w = 0;
    std::int64_t exp_n = 0;
    std::int64_t exp_c = 0;
    std::int64_t exp_h = 0;
    std::int64_t exp_w = 0;

    if(!getSemanticNchw(
        input.shape,
        input.layout,
        in_n,
        in_c,
        in_h,
        in_w
    )|| !getSemanticNchw(
        expected.shape,
        expected.layout,
        exp_n,
        exp_c,
        exp_h,
        exp_w
    ))
    {
        return false;
    }
    return in_n == exp_n &&
           in_c == exp_c &&
           in_h == exp_h &&
           in_w == exp_w;

}
}//namespace


struct RknnEngine::Impl{
    explicit Impl(std::string path) : model_path(std::move(path)){}

    std::string model_path;
    rknn_context context = 0;
    bool initialized = false;

    std::string last_error;
    std::string api_version;
    std::string driver_version;

    std::vector<TensorAttr> input_attrs;
    std::vector<TensorAttr> output_attrs;

    void destroyContext() noexcept{
        if(initialized)
        {
            rknn_destroy(context);
        }
        context = 0;
        initialized = false;
    }

    void clearMetadata() noexcept{
        api_version.clear();
        driver_version.clear();
        input_attrs.clear();
        output_attrs.clear();
    }

    bool fail(std::string message)
    {
        last_error = std::move(message);
        return false;
    }

    bool queryTensorAttrs(
        rknn_query_cmd command,
        std::uint32_t count,
        std::vector<TensorAttr>& destination
    )
    {
        destination.clear();
        destination.reserve(count);

        for(std::uint32_t i = 0; i < count; ++i)
        {
            //command 的值是 RKNN_QUERY_INPUT_ATTR 或 RKNN_QUERY_OUTPUT_ATTR
            // typedef struct _rknn_tensor_attr {
            //     uint32_t index;    // ← 输入参数：指定查询输入/输出第几个张量（调用前必须设置）
            //     uint32_t n_dims;   // ← 输出参数：维度数量（驱动填充）
            //     uint32_t dims[16]; // ← 输出参数：各维度大小（驱动填充）
            //     char name[256];    // ← 输出参数：张量名称（驱动填充）
            //     // ... 其他属性
            // } rknn_tensor_attr;

            rknn_tensor_attr attr{};
            std::memset(&attr, 0, sizeof(attr));
            attr.index = i;

            int ret = rknn_query(
                context,
                command,
                &attr,
                sizeof(attr)
            );
            if(ret != RKNN_SUCC)
            {
                return fail(makeRknnError("rknn_query(tensor_attr)", ret));
            }
            destination.push_back(convertAttr(attr));            
        }

        return true;
    }
};




RknnEngine::RknnEngine(std::string model_path) : impl_(std::make_unique<Impl>(std::move(model_path))) 
{

}
RknnEngine::~RknnEngine() {
    close();
}

RknnEngine::RknnEngine(RknnEngine&&) noexcept = default;
RknnEngine& RknnEngine::operator=(RknnEngine&&) noexcept = default;

bool RknnEngine::init()
{
    if(impl_->initialized)
    {
        return true;
    }
    impl_->last_error.clear();
    impl_->clearMetadata();

    if(impl_->model_path.empty())
    {
        return impl_->fail("model path is emplty");
    }
    // RKNN API supports passing a model path when size == 0.
    const int init_ret = rknn_init(
        &impl_->context,
        const_cast<char*>(impl_->model_path.c_str()),
        0,
        0,
        nullptr
    );
    if(init_ret != RKNN_SUCC)
    {
        impl_->context = 0;
        return impl_->fail(makeRknnError("rknn_init", init_ret));

    }
    impl_->initialized = true;
    // ============================================================
    // Register GridSample custom CPU op
    // ============================================================
    const int custom_op_ret = rkcam::platform::rockchip::rknn::registerGridSampleCustomOp(impl_->context);
    if (custom_op_ret != RKNN_SUCC)
    {
        impl_->destroyContext();
        impl_->clearMetadata();

        return impl_->fail(
            makeRknnError(
                "registerGridSampleCustomOp",
                custom_op_ret));
    }

    rknn_sdk_version sdk_version;
    std::memset(&sdk_version, 0, sizeof(sdk_version));

    int ret = rknn_query(
        impl_->context,
        RKNN_QUERY_SDK_VERSION,
        &sdk_version,
        sizeof(sdk_version)
    );

    if(ret != RKNN_SUCC)
    {
        impl_->destroyContext();
        impl_->clearMetadata();
        return impl_->fail(makeRknnError("rknn_query(SDK_VERSION)", ret));
    }

    impl_->api_version = sdk_version.api_version;
    impl_->driver_version = sdk_version.drv_version;

    rknn_input_output_num io_num{};
    std::memset(&io_num, 0, sizeof(io_num));

    ret = rknn_query(
        impl_->context,
        RKNN_QUERY_IN_OUT_NUM,
        &io_num,
        sizeof(io_num)
    );

    if(ret != RKNN_SUCC)
    {
        impl_->destroyContext();
        impl_->clearMetadata();
        return impl_->fail(makeRknnError("rknn_query(IN_OUT_NUM)", ret));
    }

    if(!impl_->queryTensorAttrs(
        RKNN_QUERY_INPUT_ATTR,
        io_num.n_input,
        impl_->input_attrs
    ))
    {
        const std::string& error = impl_->last_error;
        impl_->destroyContext();
        impl_->clearMetadata();
        return impl_->fail(error);
    }

    if(!impl_->queryTensorAttrs(
        RKNN_QUERY_OUTPUT_ATTR,
        io_num.n_output,
        impl_->output_attrs
    ))
    {
        const std::string& error = impl_->last_error;
        impl_->destroyContext();
        impl_->clearMetadata();
        return impl_->fail(error);
    }
    return true;
}


void RknnEngine::close() noexcept
{
    if(!impl_)
    {
        return;
    }
    impl_->destroyContext();
    impl_->clearMetadata();
}

bool RknnEngine::initialized() const noexcept
{
    return impl_ && impl_->initialized;
}

const std::string& RknnEngine::modelPath() const noexcept
{
    return impl_->model_path;
}


const std::string& RknnEngine::lastError() const noexcept
{
    return impl_->last_error;
}

const std::string& RknnEngine::apiVersion() const noexcept
{
    return impl_->api_version;
}
const std::string& RknnEngine::driverVersion() const noexcept
{
    return impl_->driver_version;
}

std::size_t RknnEngine::inputCount() const noexcept
{
    return impl_->input_attrs.size();
}

std::size_t RknnEngine::outputCount() const noexcept
{
    return impl_->output_attrs.size();
}

const TensorAttr& RknnEngine::inputAttr(std::size_t index) const
{
    if(index >= impl_->input_attrs.size())
    {
        throw std::out_of_range("input index out of range");
    }
    return impl_->input_attrs[index];
}

const TensorAttr& RknnEngine::outputAttr(std::size_t index) const
{
    if(index >= impl_->output_attrs.size())
    {
        throw std::out_of_range("output index out of range");
    }
    return impl_->output_attrs[index];
}


const std::vector<TensorAttr>& RknnEngine::inputAttrs() const noexcept
{
    return impl_->input_attrs;
}
const std::vector<TensorAttr>& RknnEngine::outputAttrs() const noexcept
{
    return impl_->output_attrs;
}

bool RknnEngine::infer(const FloatTensor& input, std::vector<FloatTensor>& outputs)
{
    return inferImpl(&input, 1, outputs);
}

bool RknnEngine::infer(
    const std::vector<FloatTensor>& inputs,
    std::vector<FloatTensor>& outputs
)
{
    return inferImpl(inputs.data(), inputs.size(), outputs);
}

bool RknnEngine::inferImpl(
    const FloatTensor* inputs,
    std::size_t input_count,
    std::vector<FloatTensor>& outputs
)
{
    outputs.clear();
    impl_->last_error.clear();
    if(!impl_->initialized)
    {
        return impl_->fail("RknnEngine is not initialized");
    }

    if(input_count != impl_->input_attrs.size())
    {
        std::ostringstream oss;
        oss << "input count mismatch: got " << input_count << ", expected " << impl_->input_attrs.size();
        return impl_->fail(oss.str());
    }
    if(input_count != 0 && inputs ==nullptr)
    {
        return impl_->fail("input tensor pointer is null");
    }

    std::vector<rknn_input> rknn_inputs(input_count);
    std::memset(
        rknn_inputs.data(),
        0,
        rknn_inputs.size() * sizeof(rknn_input)
    );

    for(std::size_t i = 0; i < input_count; i++)
    {
        const auto& input = inputs[i];
        const auto& expected = impl_->input_attrs[i];

        if(!input.valid())
        {
            std::ostringstream oss;
            oss << "input[" << i << "] is invalid:shape element count does not match data size";
            return impl_->fail(oss.str());
        }

        if(!shapesCompatible(input,expected))
        {
            std::ostringstream oss;
            oss << "input[" << i << "] shape/layout does not match model input";
            return impl_->fail(oss.str());
        }
        rknn_tensor_format fmt = RKNN_TENSOR_UNDEFINED;
        if(!toRknnLayout(input.layout, fmt))
        {
            std::ostringstream oss;
            oss << "input[" << i << "] has unsupported/unknown layout";
            return impl_->fail(oss.str());
        }

        const std::size_t byte_size = input.data.size() * sizeof(float);
        if(byte_size > std::numeric_limits<std::uint32_t>::max())
        {
            std::ostringstream oss;
            oss <<"input[" << i << "] is too large for RKNN input size field";
            return impl_->fail(oss.str());
        }

        auto& rknn_input = rknn_inputs[i];
        rknn_input.index = static_cast<std::uint32_t>(i);
        rknn_input.buf = const_cast<float*>(input.data.data()); //去掉 const 属性
        rknn_input.size = static_cast<std::uint32_t>(byte_size);
        rknn_input.pass_through = 0;
        rknn_input.type = RKNN_TENSOR_FLOAT32;
        rknn_input.fmt = fmt;
    }


    int ret = rknn_inputs_set(
        impl_->context,
        static_cast<std::uint32_t>(rknn_inputs.size()),
        rknn_inputs.data()
    );
    if(ret != RKNN_SUCC)
    {
        return impl_->fail(makeRknnError("rknn_inputs_set", ret));
    }

    ret = rknn_run(impl_->context, nullptr);
    if(ret != RKNN_SUCC)
    {
        return impl_->fail(makeRknnError("rknn_run", ret));
    }

    std::vector<rknn_output> rknn_outputs(impl_->output_attrs.size());
    std::memset(
        rknn_outputs.data(),
        0,
        rknn_outputs.size() * sizeof(rknn_output)
    );

    for(std::size_t i = 0; i < rknn_outputs.size(); ++i)
    {
        rknn_outputs[i].index = static_cast<std::uint32_t>(i);
        rknn_outputs[i].want_float = 1;
        rknn_outputs[i].is_prealloc = 0;
    }

    ret = rknn_outputs_get(
        impl_->context,
        static_cast<std::uint32_t>(rknn_outputs.size()),
        rknn_outputs.data(),
        nullptr
    );

    if(ret != RKNN_SUCC)
    {
        return impl_->fail(makeRknnError("rknn_outputs_get", ret));
    }

    bool copy_ok = true;
    std::string copy_error;
    try{
        outputs.reserve(rknn_outputs.size());
        for(std::size_t i = 0; i < rknn_outputs.size(); ++i)
        {
            const auto& attr = impl_->output_attrs[i];
            const auto& output = rknn_outputs[i];
            if(output.buf == nullptr)
            {
                std::ostringstream oss;
                oss << "output[" << i << "] buffer is null";
                copy_error = oss.str();
                copy_ok = false;
                break;
            }
            FloatTensor tensor;
            tensor.shape = attr.shape;
            tensor.layout = attr.layout;
            const auto* begin = static_cast<const float*>(output.buf);
            tensor.data.assign(begin, begin + attr.element_count);
            outputs.push_back(std::move(tensor));
        }
    }catch(const std::exception& e)
    {
        copy_ok = false;
        copy_error = std::string("failed to copy RKNN outputs: ") + e.what();
    }

    const int release_ret = rknn_outputs_release(
        impl_->context,
        static_cast<std::uint32_t>(rknn_outputs.size()),
        rknn_outputs.data()
    );
    if(!copy_ok)
    {
        outputs.clear();
        return impl_->fail(copy_error);
    }
    if(release_ret != RKNN_SUCC)
    {
        outputs.clear();
        return impl_->fail(makeRknnError("rknn_outputs_release", release_ret));
    }
    return true;


}

}//namespace rkcam::ai