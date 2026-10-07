#include "rkcam/ai/unaligned_hit/hit_input_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rkcam::ai::hit {

namespace{
[[nodiscard]] std::size_t imageOffset(
    int x,
    int y,
    int channel, //索引通道
    int width,
    int channels //总通道数
)
{
    //输入形状[H, W, C]
    return (
        static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
        + static_cast<std::size_t>(x)
    ) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(channel);
}

[[nodiscard]] std::uint8_t saturateU8(float value)
{
    long rounded = std::lround(value);                  // ① 四舍五入到最近的整数
    rounded = std::max(0L, std::min(255L, rounded));   // ② 夹紧到 [0, 255]
    return static_cast<std::uint8_t>(rounded);          // ③ 转成 uint8_t 返回
}

} //namespace

HiTInputProcessor::HiTInputProcessor()
    : HiTInputProcessor(Config{}) {}
HiTInputProcessor::HiTInputProcessor(const Config& config) : config_(config){
    if (config_.template_size <= 0) {
        throw std::invalid_argument("HiT template_size must be positive.");
    }
    if (config_.search_size <= 0) {
        throw std::invalid_argument("HiT search_size must be positive.");
    }
    if (!(config_.template_factor > 0.0F)) {
        throw std::invalid_argument("HiT template_factor must be positive.");
    }
    if (!(config_.search_factor > 0.0F)) {
        throw std::invalid_argument("HiT search_factor must be positive.");
    }
    for (std::size_t i = 0; i < config_.std.size(); ++i) {
        if (!std::isfinite(config_.mean[i])) {
            throw std::invalid_argument("HiT normalization mean contains non-finite value.");
        }
        if (!std::isfinite(config_.std[i]) || config_.std[i] <= 0.0F) {
            throw std::invalid_argument("HiT normalization std must be finite and positive.");
        }
    }
}

void HiTInputProcessor::validateImage(const Image& image, const char* name)
{
    if (image.width <= 0 || image.height <= 0) {
        throw std::invalid_argument(std::string(name) + ": invalid image size.");
    }
    if (image.channels != kChannelsPerModality) {
        throw std::invalid_argument(
            std::string(name) + ": HiT expects RGB888 image with channels=3."
        );
    }
    const std::size_t expected =
        static_cast<std::size_t>(image.width)
        * static_cast<std::size_t>(image.height)
        * static_cast<std::size_t>(image.channels);
    if (image.data.size() != expected) {
        throw std::invalid_argument(
            std::string(name) + ": image.data size does not match width*height*channels."
        );
    }
}

void HiTInputProcessor::validateBox(const TrackingBox& box, const char* name) {
    const bool finite =
        std::isfinite(box.x)
        && std::isfinite(box.y)
        && std::isfinite(box.w)
        && std::isfinite(box.h);
    if (!finite) {
        throw std::invalid_argument(std::string(name) + ": bbox contains non-finite value.");
    }
    if (box.w <= 0.0F || box.h <= 0.0F) {
        throw std::invalid_argument(std::string(name) + ": bbox width/height must be positive.");
    }
}

int HiTInputProcessor::pythonRoundToInt(double value)
{
    if(!std::isfinite(value))
    {
        throw std::invalid_argument("pythonRoundToInt received non-finite value.");
    }
    // Python round() uses round-half-to-even.  Avoid std::round(), whose tie
    // rule is away from zero and can move the crop origin by one pixel.
    const double floor_value = std::floor(value);
    const double fraction = value - floor_value;
    if(fraction < 0.5)
    {
        return static_cast<int>(floor_value);
    }
    if(fraction > 0.5)
    {
        return static_cast<int>(floor_value + 1.0);
    }
    const auto base = static_cast<long long>(floor_value);
    return static_cast<int>(base % 2LL == 0LL ? base : base + 1LL);
}

int HiTInputProcessor::maxSafeCropSize(const Image& image) {
    return std::min(
        kMaxCropSide,
        kMaxCropToImageSide * std::max(image.width, image.height)
    );
}
HiTInputProcessor::CropResult HiTInputProcessor::sampleTarget(
    const Image& image,
    const TrackingBox& target_box, //注意，这个target_box是坐标从0开始的
    float search_area_factor,
    int output_size
)
{
    //注意，这里的裁剪不能使用opencv的库函数，因为要严格复刻python端的实现，opencv的实现不一定和python一样
    validateImage(image, "sampleTarget image");
    validateBox(target_box, "sampleTarget target_box");
    if (!std::isfinite(search_area_factor) || search_area_factor <= 0.0F) {
        throw std::invalid_argument("sampleTarget search_area_factor must be positive.");
    }
    if (output_size <= 0) {
        throw std::invalid_argument("sampleTarget output_size must be positive.");
    }
    //注意，这里因为最终要裁的是正方形，所以必须根据sqrt(面积)后的等效边长来进行缩放
    const double area = static_cast<double>(target_box.w) * static_cast<double>(target_box.h);
    const int crop_size = static_cast<int>(std::ceil(std::sqrt(area) * static_cast<double>(search_area_factor)));
    if (crop_size < 1) {
        throw std::runtime_error("sampleTarget produced crop_size < 1.");
    }
    if (crop_size > maxSafeCropSize(image)) {
        throw std::runtime_error("sampleTarget requested crop exceeds Python safety limit.");
    }
    const int x1 = pythonRoundToInt(
        static_cast<double>(target_box.x) + 0.5 * static_cast<double>(target_box.w)
        - 0.5 * static_cast<double>(crop_size)
    );
    const int y1 = pythonRoundToInt(
        static_cast<double>(target_box.y)
        + 0.5 * static_cast<double>(target_box.h)
        - 0.5 * static_cast<double>(crop_size)
    );
    const int x2 = x1 + crop_size;
    const int y2 = y1 + crop_size;
    // Keep the historical PMATrack/sample_target behavior exactly, including
    // the +1 on right/bottom padding.
    //在 NumPy 里,# 切片是"左闭右开"的
    // crop = img[y1:y2, x1:x2]
    // 表示取 y1 到 y2-1，x1 到 x2-1
    const int x1_pad = std::max(0, -x1); //x1_pad = max(0, -(-10)) = 10（左边超 10 像素）
    const int x2_pad = std::max(x2 - image.width + 1, 0);
    const int y1_pad = std::max(0, -y1);
    const int y2_pad = std::max(y2 - image.height + 1, 0);

    Image padded;
    padded.width = crop_size;
    padded.height = crop_size;
    padded.channels = kChannelsPerModality;
    padded.data.assign(
        static_cast<std::size_t>(crop_size) *
        static_cast<std::size_t>(crop_size) *
        kChannelsPerModality,
        0
    );
    const int src_x_begin = x1 + x1_pad;
    const int src_x_end = x2 - x2_pad;
    const int src_y_begin = y1 + y1_pad;
    const int src_y_end = y2 - y2_pad;
    if(src_x_begin < src_x_end && src_y_begin < src_y_end)
    {
        for(int sy = src_y_begin; sy < src_y_end; ++sy)
        {
            const int dy = y1_pad + (sy - src_y_begin);
            if(sy < 0 || sy >= image.height || dy < 0 || dy>= crop_size)
            {
                continue;
            }
            for(int sx = src_x_begin; sx < src_x_end; ++sx)
            {
                const int dx = x1_pad + (sx - src_x_begin);
                if(sx < 0 || sx >= image.width || dx < 0 || dx >= crop_size)
                {
                    continue;
                }
                for(int c = 0; c < kChannelsPerModality; ++c)
                {
                    padded.data[imageOffset(dx, dy, c, crop_size, kChannelsPerModality)] =
                        image.data[imageOffset(sx, sy, c, image.width, image.channels)];
                }
            }
        }
    }

    CropResult result;
    result.patch = resizeBilinearRgb(padded, output_size, output_size);
    result.resize_factor = static_cast<float>(output_size) / static_cast<float>(crop_size);
    return result;
}   
Image HiTInputProcessor::resizeBilinearRgb(
    const Image& src,
    int dst_width,
    int dst_height
) {
    validateImage(src, "resizeBilinearRgb src");
    if (dst_width <= 0 || dst_height <= 0) {
        throw std::invalid_argument("resizeBilinearRgb destination size must be positive.");
    }

    if (src.width == dst_width && src.height == dst_height) {
        return src;
    }

    Image dst;
    dst.width = dst_width;
    dst.height = dst_height;
    dst.channels = kChannelsPerModality;
    dst.data.resize(
        static_cast<std::size_t>(dst_width)
        * static_cast<std::size_t>(dst_height)
        * kChannelsPerModality
    );

    const float scale_x = static_cast<float>(src.width) / static_cast<float>(dst_width);
    const float scale_y = static_cast<float>(src.height) / static_cast<float>(dst_height);

    for (int dy = 0; dy < dst_height; ++dy) {
        const float src_y = (static_cast<float>(dy) + 0.5F) * scale_y - 0.5F;
        int y0 = static_cast<int>(std::floor(src_y));
        int y1 = y0 + 1;
        const float wy = src_y - static_cast<float>(y0);
        y0 = std::clamp(y0, 0, src.height - 1);
        y1 = std::clamp(y1, 0, src.height - 1);

        for (int dx = 0; dx < dst_width; ++dx) {
            const float src_x = (static_cast<float>(dx) + 0.5F) * scale_x - 0.5F;
            int x0 = static_cast<int>(std::floor(src_x));
            int x1 = x0 + 1;
            const float wx = src_x - static_cast<float>(x0);
            x0 = std::clamp(x0, 0, src.width - 1);
            x1 = std::clamp(x1, 0, src.width - 1);

            for (int c = 0; c < kChannelsPerModality; ++c) {
                const float p00 = static_cast<float>(
                    src.data[imageOffset(x0, y0, c, src.width, src.channels)]
                );
                const float p01 = static_cast<float>(
                    src.data[imageOffset(x1, y0, c, src.width, src.channels)]
                );
                const float p10 = static_cast<float>(
                    src.data[imageOffset(x0, y1, c, src.width, src.channels)]
                );
                const float p11 = static_cast<float>(
                    src.data[imageOffset(x1, y1, c, src.width, src.channels)]
                );

                const float top = p00 + (p01 - p00) * wx;
                const float bottom = p10 + (p11 - p10) * wx;
                const float value = top + (bottom - top) * wy;

                dst.data[imageOffset(dx, dy, c, dst.width, dst.channels)] =
                    saturateU8(value);
            }
        }
    }

    return dst;
}

Homography3x3 HiTInputProcessor::buildReferenceHomography(
    const TrackingBox& init_rgb_bbox,
    const TrackingBox& init_tir_bbox
) {
    //TIR 坐标系 → RGB 坐标系
    validateBox(init_rgb_bbox, "init_rgb_bbox");
    validateBox(init_tir_bbox, "init_tir_bbox");

    const float scale_x = init_rgb_bbox.w / init_tir_bbox.w;
    const float scale_y = init_rgb_bbox.h / init_tir_bbox.h;
    const float translate_x = init_rgb_bbox.x - scale_x * init_tir_bbox.x;
    const float translate_y = init_rgb_bbox.y - scale_y * init_tir_bbox.y;

    Homography3x3 h;
    h.data = {
        scale_x, 0.0F, translate_x,
        0.0F, scale_y, translate_y,
        0.0F, 0.0F, 1.0F,
    };
    return h;
}

Homography3x3 HiTInputProcessor::buildAlignedCropMatrix(
    const TrackingBox& target_box,
    float search_area_factor,
    int output_size
) {
	////原图坐标系 → 裁剪+resize 后的输出坐标系
    validateBox(target_box, "buildAlignedCropMatrix target_box");
    if (!std::isfinite(search_area_factor) || search_area_factor <= 0.0F) {
        throw std::invalid_argument("buildAlignedCropMatrix factor must be positive.");
    }
    if (output_size <= 0) {
        throw std::invalid_argument("buildAlignedCropMatrix output_size must be positive.");
    }

    const int crop_size = static_cast<int>(
        std::ceil(
            std::sqrt(
                static_cast<double>(target_box.w)
                * static_cast<double>(target_box.h)
            ) * static_cast<double>(search_area_factor)
        )
    );
    if (crop_size < 1) {
        throw std::runtime_error("buildAlignedCropMatrix produced crop_size < 1.");
    }

    const int x1 = pythonRoundToInt(
        static_cast<double>(target_box.x)
        + 0.5 * static_cast<double>(target_box.w)
        - 0.5 * static_cast<double>(crop_size)
    );
    const int y1 = pythonRoundToInt(
        static_cast<double>(target_box.y)
        + 0.5 * static_cast<double>(target_box.h)
        - 0.5 * static_cast<double>(crop_size)
    );

    const float scale = static_cast<float>(output_size) / static_cast<float>(crop_size);

    Homography3x3 matrix;
    matrix.data = {
        scale,
        0.0F,
        -scale * static_cast<float>(x1) + 0.5F * scale - 0.5F,
        0.0F,
        scale,
        -scale * static_cast<float>(y1) + 0.5F * scale - 0.5F,
        0.0F,
        0.0F,
        1.0F,
    };
    return matrix;
}
Homography3x3 HiTInputProcessor::multiply(
    const Homography3x3& a,
    const Homography3x3& b
)
{
    Homography3x3 out;
    out.data.fill(0.0f);
    for(int row = 0; row < 3; ++row)
    {
        for(int col = 0; col < 3; ++col)
        {
            float sum = 0.0f;
            for(int k = 0; k < 3; ++k)
            {
                sum += a.data[row * 3 + k] * b.data[k * 3 + col];
            }
            out.data[row * 3 + col] = sum;
        }
    }
    return out;
}
Homography3x3 HiTInputProcessor::inverse(const Homography3x3& matrix) {
    const auto& m = matrix.data;
    const float det =
        m[0] * (m[4] * m[8] - m[5] * m[7])
        - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6]);

    if (!std::isfinite(det) || std::fabs(det) < 1e-12F) {
        throw std::runtime_error("HiT homography is singular.");
    }

    const float inv_det = 1.0F / det;
    Homography3x3 out;
    out.data = {
        (m[4] * m[8] - m[5] * m[7]) * inv_det,
        (m[2] * m[7] - m[1] * m[8]) * inv_det,
        (m[1] * m[5] - m[2] * m[4]) * inv_det,
        (m[5] * m[6] - m[3] * m[8]) * inv_det,
        (m[0] * m[8] - m[2] * m[6]) * inv_det,
        (m[2] * m[3] - m[0] * m[5]) * inv_det,
        (m[3] * m[7] - m[4] * m[6]) * inv_det,
        (m[1] * m[6] - m[0] * m[7]) * inv_det,
        (m[0] * m[4] - m[1] * m[3]) * inv_det,
    };
    return out;
}
Image HiTInputProcessor::sampleAlignedTir(
    const Image& tir,
    const Homography3x3& reference_h, //TIR 原图坐标  →  RGB 原图坐标
    const TrackingBox& rgb_state,
    float search_area_factor,
    int output_size
) {
    validateImage(tir, "sampleAlignedTir tir");
    validateBox(rgb_state, "sampleAlignedTir rgb_state");

    // Python:
    //   tir_to_crop = aligned_crop_matrix(rgb_state, ...) @ reference_h
    //   cv2.warpPerspective(tir, tir_to_crop, ... INTER_LINEAR,
    //                       BORDER_CONSTANT, 0)

    const Homography3x3 aligned_to_crop = buildAlignedCropMatrix(
        rgb_state,
        search_area_factor,
        output_size
    );
    // reference_h    : TIR 原图  →  RGB 原图 Htir->rgb
    // aligned_to_crop: RGB 原图  →  裁剪空间 Hrgb->crop
    // 两者相乘        : TIR 原图  →  裁剪空间 Hrgb->crop * Htir->rgb
    //这里要逆着读P2 = H2H1P1
    //P1经过H1再经过H2变换，而不是先进行H2变换再变成H1变换
    const Homography3x3 tir_to_crop = multiply(aligned_to_crop, reference_h);
    const Homography3x3 crop_to_tir = inverse(tir_to_crop);

    Image output;
    output.width = output_size;
    output.height = output_size;
    output.channels = kChannelsPerModality;
    output.data.assign(
        static_cast<std::size_t>(output_size)
            * static_cast<std::size_t>(output_size)
            * kChannelsPerModality,
        0
    );

    const auto& inv = crop_to_tir.data;

    for (int dy = 0; dy < output_size; ++dy) {
        for (int dx = 0; dx < output_size; ++dx) {
            const float x = static_cast<float>(dx);
            const float y = static_cast<float>(dy);

            const float sx_h = inv[0] * x + inv[1] * y + inv[2];
            const float sy_h = inv[3] * x + inv[4] * y + inv[5];
            const float sw = inv[6] * x + inv[7] * y + inv[8];

            if (!std::isfinite(sw) || std::fabs(sw) < 1e-12F) {
                continue;
            }

            const float sx = sx_h / sw;
            const float sy = sy_h / sw;
            if (!std::isfinite(sx) || !std::isfinite(sy)) {
                continue;
            }

            const int x0 = static_cast<int>(std::floor(sx));
            const int y0 = static_cast<int>(std::floor(sy));
            const int x1 = x0 + 1;
            const int y1 = y0 + 1;
            const float wx = sx - static_cast<float>(x0);
            const float wy = sy - static_cast<float>(y0);

            for (int c = 0; c < kChannelsPerModality; ++c) {
                const auto sample = [&](int px, int py) -> float {
                    if (px < 0 || px >= tir.width || py < 0 || py >= tir.height) {
                        return 0.0F;
                    }
                    return static_cast<float>(
                        tir.data[imageOffset(px, py, c, tir.width, tir.channels)]
                    );
                };

                const float p00 = sample(x0, y0);
                const float p01 = sample(x1, y0);
                const float p10 = sample(x0, y1);
                const float p11 = sample(x1, y1);

                const float top = p00 + (p01 - p00) * wx;
                const float bottom = p10 + (p11 - p10) * wx;
                const float value = top + (bottom - top) * wy;

                output.data[imageOffset(dx, dy, c, output.width, output.channels)] =
                    saturateU8(value);
            }
        }
    }

    return output;
}

Image HiTInputProcessor::concatRgbTir(
    const Image& rgb_patch,
    const Image& tir_patch
) {
    validateImage(rgb_patch, "concatRgbTir rgb_patch");
    validateImage(tir_patch, "concatRgbTir tir_patch");
    if (rgb_patch.width != tir_patch.width || rgb_patch.height != tir_patch.height) {
        throw std::invalid_argument("concatRgbTir requires identical RGB/TIR patch sizes.");
    }

    Image merged;
    merged.width = rgb_patch.width;
    merged.height = rgb_patch.height;
    merged.channels = kMergedChannels;
    merged.data.resize(
        static_cast<std::size_t>(merged.width)
        * static_cast<std::size_t>(merged.height)
        * kMergedChannels
    );

    for (int y = 0; y < merged.height; ++y) {
        for (int x = 0; x < merged.width; ++x) {
            const std::size_t merged_base =
                (static_cast<std::size_t>(y) * merged.width + x) * kMergedChannels;
            const std::size_t rgb_base =
                (static_cast<std::size_t>(y) * rgb_patch.width + x) * kChannelsPerModality;
            const std::size_t tir_base =
                (static_cast<std::size_t>(y) * tir_patch.width + x) * kChannelsPerModality;

            merged.data[merged_base + 0] = rgb_patch.data[rgb_base + 0];
            merged.data[merged_base + 1] = rgb_patch.data[rgb_base + 1];
            merged.data[merged_base + 2] = rgb_patch.data[rgb_base + 2];
            merged.data[merged_base + 3] = tir_patch.data[tir_base + 0];
            merged.data[merged_base + 4] = tir_patch.data[tir_base + 1];
            merged.data[merged_base + 5] = tir_patch.data[tir_base + 2];
        }
    }

    return merged;
}
std::vector<float> HiTInputProcessor::normalizeToNchw(
    const Image& merged_patch
) const
{
    //从[H,W,C]转换到[N,C,H,W]，并且进行归一化
    //图像预处理归一化
    // 第 1 步：除以 255          → 把 [0, 255] 映射到 [0, 1]（缩放）
    // 第 2 步：减均值、除标准差   → 变成均值 0、方差 1（标准化）
    //对每个通道 c（R、G、B），求所有像素的平均值：
    //mean[c] = (1 / (N * H * W)) * Σ Σ Σ pixel[i, y, x, c]
    if (merged_patch.width <= 0 || merged_patch.height <= 0) {
        throw std::invalid_argument("normalizeToNchw received invalid image size.");
    }
    if (merged_patch.channels != kMergedChannels) {
        throw std::invalid_argument("normalizeToNchw expects a 6-channel merged patch.");
    }

    const int width = merged_patch.width;
    const int height = merged_patch.height;
    const std::size_t plane =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

    std::vector<float> output(plane * kMergedChannels);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel_base =
                (static_cast<std::size_t>(y) * width + x) * kMergedChannels;
            const std::size_t spatial = static_cast<std::size_t>(y) * width + x;

            for (int c = 0; c < kMergedChannels; ++c) {
                const int stats_channel = c % kChannelsPerModality;
                const float raw = static_cast<float>(merged_patch.data[pixel_base + c]);
                const float scaled = raw / 255.0F;
                output[static_cast<std::size_t>(c) * plane + spatial] =
                    (scaled - config_.mean[stats_channel]) / config_.std[stats_channel];
            }
        }
    }

    return output;
}
HiTPatch HiTInputProcessor::makeHiTPatch(
    const Image& rgb_patch,
    const Image& tir_patch,
    float resize_factor
) const {
    const Image merged = concatRgbTir(rgb_patch, tir_patch);

    HiTPatch result;
    result.width = merged.width;
    result.height = merged.height;
    result.channels = merged.channels;
    result.resize_factor = resize_factor;
    result.raw_hwc = merged.data;
    result.tensor_nchw = normalizeToNchw(merged);
    return result;
}

TemplateGeometry HiTInputProcessor::buildTemplateGeometry(
    const TrackingBox& rgb_box,
    const int rgb_image_width,
    const int rgb_image_height,
    float template_resize_factor
)
{
    validateBox(rgb_box, "buildTemplateGeometry rgb_box");
    if (rgb_image_width <= 0 || rgb_image_height <= 0) {
        throw std::invalid_argument("buildTemplateGeometry invalid RGB image size.");
    }
    if (!std::isfinite(template_resize_factor) || template_resize_factor <= 0.0F) {
        throw std::invalid_argument("buildTemplateGeometry invalid template resize factor.");
    }
    constexpr float eps = 1e-6f;
    TemplateGeometry geometry;
    geometry.values = {
        std::log(std::max(rgb_box.w / static_cast<float>(rgb_image_width), eps)),
        std::log(std::max(rgb_box.h / static_cast<float>(rgb_image_height), eps)),
        std::log(std::max(rgb_box.w / rgb_box.h, eps)),
        std::log(std::max(template_resize_factor, eps)),
    };
    for (float value : geometry.values) {
        if (!std::isfinite(value)) {
            throw std::runtime_error("buildTemplateGeometry generated non-finite geometry.");
        }
    }
    return geometry;
}

HiTInitializationInput HiTInputProcessor::prepareInitialization(
    const Image& rgb,
    const Image& tir,
    const TrackingBox& init_rgb_bbox,
    const TrackingBox& init_tir_bbox
) const {
    validateImage(rgb, "prepareInitialization rgb");
    validateImage(tir, "prepareInitialization tir");
    validateBox(init_rgb_bbox, "prepareInitialization init_rgb_bbox");
    validateBox(init_tir_bbox, "prepareInitialization init_tir_bbox");

    // Important: the validated Python path does NOT H0-warp the first-frame
    // TIR template. RGB and TIR T0 are cropped independently from their own
    // initialization boxes.
    const CropResult rgb_template = sampleTarget(
        rgb,
        init_rgb_bbox,
        config_.template_factor,
        config_.template_size
    );
    const CropResult tir_template = sampleTarget(
        tir,
        init_tir_bbox,
        config_.template_factor,
        config_.template_size
    );

    HiTInitializationInput output;
    output.static_template = makeHiTPatch(
        rgb_template.patch,
        tir_template.patch,
        rgb_template.resize_factor
    );
    output.reference_h = buildReferenceHomography(init_rgb_bbox, init_tir_bbox);
    output.template_geometry = buildTemplateGeometry(
        init_rgb_bbox,
        rgb.width,
        rgb.height,
        rgb_template.resize_factor
    );
    return output;
}
HiTSearchInput HiTInputProcessor::prepareSearch(
    const Image& rgb,
    const Image& tir,
    const TrackingBox& rgb_state,
    const Homography3x3& reference_h
)const
{
    validateImage(rgb, "prepareSearch rgb");
    validateImage(tir, "prepareSearch tir");
    validateBox(rgb_state, "prepareSearch rgb_state");
    
    const CropResult rgb_search = sampleTarget(
        rgb,
        rgb_state,
        config_.search_factor,
        config_.search_size
    );
    const Image tir_search = sampleAlignedTir(
        tir,
        reference_h,
        rgb_state,
        config_.search_factor,
        config_.search_size
    );
    HiTSearchInput output;
    output.search = makeHiTPatch(
        rgb_search.patch,
        tir_search,
        rgb_search.resize_factor
    );
    // Match the final Python GTRF runner: IR reports the same crop resize
    // factor because it is sampled directly into the RGB-reference crop.
    output.resize_factor_rgb = rgb_search.resize_factor;
    output.resize_factor_ir = rgb_search.resize_factor;
    return output;
}
HiTDynamicTemplateInput HiTInputProcessor::prepareDynamicTemplate(
    const Image& rgb,
    const Image& tir,
    const TrackingBox& predicted_rgb_state,
    const Homography3x3& reference_h,
    int source_frame
) const {
    validateImage(rgb, "prepareDynamicTemplate rgb");
    validateImage(tir, "prepareDynamicTemplate tir");
    validateBox(predicted_rgb_state, "prepareDynamicTemplate predicted_rgb_state");

    const CropResult rgb_dynamic = sampleTarget(
        rgb,
        predicted_rgb_state,
        config_.template_factor,
        config_.template_size
    );
    const Image tir_dynamic = sampleAlignedTir(
        tir,
        reference_h,
        predicted_rgb_state,
        config_.template_factor,
        config_.template_size
    );

    HiTDynamicTemplateInput output;
    output.dynamic_template = makeHiTPatch(
        rgb_dynamic.patch,
        tir_dynamic,
        rgb_dynamic.resize_factor
    );
    output.source_frame = source_frame;
    return output;
}

} //namespace rkcam::ai::hit