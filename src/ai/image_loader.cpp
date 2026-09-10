#include "rkcam/ai/image_loader.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

// stb_image 的实现只能在整个工程中定义一次。
//
// 如果以后工程中已经有一个专门的 stb_image_impl.cpp，
// 那么这里就不能再定义 STB_IMAGE_IMPLEMENTATION，
// 应该把这个宏移到那个 cpp 中。
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rkcam::ai{
namespace{
// ------------------------------------------------------------
// stb_image 返回的是裸指针，需要调用 stbi_image_free()。
// 用 unique_ptr 包装，确保所有异常路径都不会发生内存泄漏。
// ------------------------------------------------------------
struct StbiImageDeleter{

    void operator()(stbi_uc* ptr) const noexcept
    {
        if(ptr != nullptr)
        {
            stbi_image_free(ptr);
        }
    }
};

using StbiImagePtr = std::unique_ptr<stbi_uc, StbiImageDeleter>;

constexpr int kRgbChannels = 3;

// ------------------------------------------------------------
// 安全计算：width * height * channels
//
// 防止异常图片尺寸导致 size_t 整数溢出。
// ------------------------------------------------------------

std::size_t calculateImageBytes(
    int width,
    int height,
    int channels
)
{
    if(width <= 0||
        height <= 0 ||
        channels <=0
    )
    {
        throw std::runtime_error("invalid image dimensions");
    }

    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    const auto c = static_cast<std::size_t>(channels);

    constexpr auto kMax = std::numeric_limits<std::size_t>::max();

    //width * height
    if(w > kMax / h)
    {
        throw std::runtime_error("image size overflow: width * height");
    }
    const std::size_t pixels = w * h;
    //pixels * channels
    if(pixels > kMax / c)
    {
        throw std::runtime_error("image size overflow: pixels * channels");
    }
    return pixels * c;

}

}    //namespace

Image ImageLoader::loadRgb(const std::string& path)
{
    if(path.empty())
    {
        throw std::invalid_argument("ImageLoader::loadRgb: path is empty");
    }

    int width = 0;
    int height = 0;

    // 文件原始通道数。
    //
    // 注意：
    // source_channels 只是告诉我们源文件是多少通道。
    //
    // 因为 stbi_load 最后一个参数指定为 3，
    // 所以真正返回的数据始终为 RGB 三通道。
    int source_channels = 0;

    stbi_uc* raw_data = stbi_load(
        path.c_str(),
        &width,
        &height,
        &source_channels,
        kRgbChannels
    );

    if(raw_data == nullptr)
    {
        const char* reason = stbi_failure_reason();
        std::string message = "ImageLoader::loadRgb: failed to load image: " + path;

        if(reason != nullptr)
        {
            message += ", reason: ";
            message += reason;
        }

        throw std::runtime_error(message);
    }
    //从这里开始交给RAII管理
    StbiImagePtr image_data(raw_data);
    if(width <= 0 || height <= 0)
    {
        throw std::runtime_error("ImageLoader::loadRgb: invalid image dimension: " + path);
    }

    const std::size_t byte_count = calculateImageBytes(
        width,
        height,
        kRgbChannels
    );
    Image image;
    image.width = width;
    image.height = height;
    image.channels = kRgbChannels;

    // stb_image 返回：
    //
    // RGBRGBRGB...
    //
    // 直接复制到我们统一定义的 Image.data 中。

    image.data.assign(image_data.get(), image_data.get() + byte_count);
    if(!image.valid())
    {
        throw std::runtime_error(
            "ImageLoader::loadRgb: decoded image is invalid: " + path
        );
    }

    return image;

}

}