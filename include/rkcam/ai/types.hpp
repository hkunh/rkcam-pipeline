#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rkcam::ai {

enum class TensorLayout{
    //N=批次, C=通道, H=高, W=宽

    NCHW,
    NHWC,
    // 分块排列格式，常见于华为昇腾等NPU硬件
    // 把C通道维度拆成C1个块，每块固定C2个通道（如C2=16）
    // 形状为[N, C1, H, W, C2]
    // 目的是让数据排列贴合硬件的向量计算单元，提升内存访问效率
    NC1HWC2,
    Unknown,
};

enum class TensorDataType{

    Float32,
    Float16,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Bool,
    Unknown,
};

enum class TensorQuantizationType{
    None,
    Dfp, // Dynamic Fixed Point 动态定点: 一个张量里的所有元素共享一个指数，每个元素只存一个整数
    AffineAsymmetric, // Affine Asymmetric Quantization 仿射非对称量化: 用缩放因子 S 和零点 Z 两个参数，把浮点范围线性映射到整数范围
    Unknown,
};

struct TensorShape{
    std::vector<int64_t> dims;
 
    // [[nodiscard]]: C++17引入的属性说明符（attribute），意思是"调用者不能忽略这个函数的返回值"。
    [[nodiscard]] std::size_t rank() const noexcept{
        return dims.size();
    }

    [[nodiscard]] std::size_t elementCount() const noexcept{
        if(dims.empty())
        {
            return 0;
        }
        std::size_t count = 1;
        for(const auto dim : dims)
        {
            if(dim <= 0)
            {
                return 0;
            }
            count *= static_cast<std::size_t>(dim);
        }
        return count;
    };

    [[nodiscard]] bool empty() const noexcept{
        return dims.empty();
    }
};


struct TensorAttr{

    std::uint32_t index = 0;
    
    std::string name;
    TensorShape shape;
    TensorLayout layout = TensorLayout::Unknown; // 内存排列方式，如NCHW、NHWC
    TensorDataType data_type = TensorDataType::Unknown;
    TensorQuantizationType quantization_type = TensorQuantizationType::Unknown;

    std::size_t element_count = 0;
    std::size_t byte_size = 0;
    std::size_t byte_size_with_stride = 0;

    std::int32_t zero_point = 0; // 量化零点Z，反量化公式中的偏移量
    float scale = 1.0f;  // 量化缩放因子S，反量化公式中的比例系数
};

struct FloatTensor{
    TensorShape shape;
    TensorLayout layout = TensorLayout::Unknown;
    std::vector<float> data;
    [[nodiscard]] bool valid() const noexcept{
        const auto expected = shape.elementCount();
        return expected != 0 && expected == data.size();
    }
};


struct Image{
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> data;

    [[nodiscard]] bool valid() const noexcept{
        if(width <= 0 || height <= 0 || channels <= 0)
        {
            return false;
        }
        const auto expected = static_cast<std::size_t>(width) * 
                              static_cast<std::size_t>(height) * 
                              static_cast<std::size_t>(channels);
        return data.size() == expected;
    }

};


struct PreprocessInfo{
    int original_width = 0;
    int original_height = 0;
    
    int model_width = 0;
    int model_height = 0;

    //   scale_x =
    //       model_w / original_w
    //
    //   scale_y =
    //       model_h / original_h
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    float pad_x = 0.0f;
    float pad_y = 0.0f;

};

struct Detection{

    int class_id = -1;
    float score = 0.0f;
    
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};




} //namespace rkcam::ai