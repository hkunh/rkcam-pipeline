#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rkcam::ai::hit {

struct TrackingBox {
    float x = 0.0F;
    float y = 0.0F;
    float w = 0.0F;
    float h = 0.0F;
};

struct Homography3x3 {
    // Row-major 3x3 matrix.
    std::array<float, 9> data{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F,
    };
};

struct TemplateGeometry {
    // [
    //   log(target_w / image_w),
    //   log(target_h / image_h),
    //   log(target_w / target_h),
    //   log(template_resize_factor),
    // ]
    std::array<float, 4> values{};
};

/**
 * @brief One 6-channel HiT crop in both debug/raw and model-ready forms.
 *
 * raw_hwc:
 *   H x W x 6 uint8, channel order:
 *   [RGB-R, RGB-G, RGB-B, TIR-R, TIR-G, TIR-B]
 *
 * tensor_nchw:
 *   [1, 6, H, W] float32, normalized with DATA.MEAN / DATA.STD.
 */
struct HiTPatch {
    int width = 0;
    int height = 0;
    int channels = 6;
    float resize_factor = 1.0F;

    std::vector<std::uint8_t> raw_hwc;
    std::vector<float> tensor_nchw;
};

/**
 * @brief Sequence initialization products owned by upper tracker state.
 *
 * The processor computes these once. Their lifetime is NOT owned by the
 * processor itself.
 */
struct HiTInitializationInput {
    HiTPatch static_template;
    Homography3x3 reference_h;
    TemplateGeometry template_geometry;
};

struct HiTSearchInput {
    HiTPatch search;
    float resize_factor_rgb = 1.0F;
    float resize_factor_ir = 1.0F;
};

struct HiTDynamicTemplateInput {
    HiTPatch dynamic_template;
    int source_frame = -1;
};

}  // namespace rkcam::ai::hit
