#pragma once

#include <array>

#include "rkcam/ai/types.hpp"
#include "rkcam/ai/unaligned_hit/hit_types.hpp"

namespace rkcam::ai::hit {

/**
 * @brief Input preprocessor for the final LasHeR-Unaligned HiT tracker.
 *
 * This class reproduces the validated Python inference preprocessing used by:
 *   fixed first-frame H0 + GSC + HLFR-V2 + Memory-M2.
 *
 * Responsibilities:
 *   - first-frame static RGB/TIR template crop;
 *   - fixed first-frame native-TIR -> RGB H0 construction;
 *   - GSC first-frame template geometry construction;
 *   - RGB-reference search crop;
 *   - fixed-H0 TIR search sampling;
 *   - online dynamic-template crop generation;
 *   - RGB/TIR concat to 6 channels;
 *   - /255 + mean/std normalization to [1,6,H,W].
 *
 * Not responsible for:
 *   - DynamicTemplateManager update policy / interval;
 *   - RKNN execution;
 *   - HiT postprocess / bbox mapping;
 *   - tracker state lifetime.
 */
class HiTInputProcessor final {
public:
    struct Config {
        int template_size = 128;
        float template_factor = 2.0F;

        int search_size = 256;
        float search_factor = 4.0F;

        std::array<float, 3> mean{0.485F, 0.456F, 0.406F};
        std::array<float, 3> std{0.229F, 0.224F, 0.225F};
    };

    HiTInputProcessor();
    explicit HiTInputProcessor(const Config& config);

    /**
     * @brief Build sequence-fixed initialization state from frame 0.
     *
     * Python-equivalent behavior:
     *   RGB T0: sample_target(rgb0, init_rgb_bbox, TEMPLATE_FACTOR, 128)
     *   TIR T0: sample_target(tir0, init_tir_bbox, TEMPLATE_FACTOR, 128)
     *   H0:     rectangle_alignment_h(init_rgb_bbox, init_tir_bbox)
     *   GSC:    first-frame RGB geometry, fixed for the whole sequence
     */
    [[nodiscard]] HiTInitializationInput prepareInitialization(
        const Image& rgb,
        const Image& tir,
        const TrackingBox& init_rgb_bbox,
        const TrackingBox& init_tir_bbox
    ) const;

    /**
     * @brief Build frame-t search input around the previous/current RGB state.
     *
     * RGB:
     *   normal sample_target(..., SEARCH_FACTOR, 256)
     *
     * TIR:
     *   one direct fixed-H0 sampling operation equivalent to
     *   cv2.warpPerspective(tir, aligned_crop_matrix @ H0, ...).
     */
    [[nodiscard]] HiTSearchInput prepareSearch(
        const Image& rgb,
        const Image& tir,
        const TrackingBox& rgb_state,
        const Homography3x3& reference_h
    ) const;

    /**
     * @brief Build a Memory-M2 dynamic template from an already tracked frame.
     *
     * This only constructs Td. Whether/when Td replaces the previous Td is the
     * DynamicTemplateManager's responsibility.
     */
    [[nodiscard]] HiTDynamicTemplateInput prepareDynamicTemplate(
        const Image& rgb,
        const Image& tir,
        const TrackingBox& predicted_rgb_state,
        const Homography3x3& reference_h,
        int source_frame
    ) const;

    [[nodiscard]] static Homography3x3 buildReferenceHomography(
        const TrackingBox& init_rgb_bbox,
        const TrackingBox& init_tir_bbox
    );

private:
    struct CropResult {
        Image patch;
        float resize_factor = 1.0F;
    };

    Config config_;

    static constexpr int kChannelsPerModality = 3;
    static constexpr int kMergedChannels = 6;
    static constexpr int kMaxCropSide = 4096;
    static constexpr int kMaxCropToImageSide = 4;

    static void validateImage(const Image& image, const char* name);
    static void validateBox(const TrackingBox& box, const char* name);

    [[nodiscard]] static int pythonRoundToInt(double value);
    [[nodiscard]] static int maxSafeCropSize(const Image& image);

    [[nodiscard]] static CropResult sampleTarget(
        const Image& image,
        const TrackingBox& target_box,
        float search_area_factor,
        int output_size
    );

    [[nodiscard]] static Image resizeBilinearRgb(
        const Image& src,
        int dst_width,
        int dst_height
    );

    [[nodiscard]] static Homography3x3 buildAlignedCropMatrix(
        const TrackingBox& target_box,
        float search_area_factor,
        int output_size
    );

    [[nodiscard]] static Image sampleAlignedTir(
        const Image& tir,
        const Homography3x3& reference_h,
        const TrackingBox& rgb_state,
        float search_area_factor,
        int output_size
    );

    [[nodiscard]] static Homography3x3 multiply(
        const Homography3x3& a,
        const Homography3x3& b
    );

    [[nodiscard]] static Homography3x3 inverse(
        const Homography3x3& matrix
    );

    [[nodiscard]] static Image concatRgbTir(
        const Image& rgb_patch,
        const Image& tir_patch
    );

    [[nodiscard]] std::vector<float> normalizeToNchw(
        const Image& merged_patch
    ) const;

    [[nodiscard]] HiTPatch makeHiTPatch(
        const Image& rgb_patch,
        const Image& tir_patch,
        float resize_factor
    ) const;

    [[nodiscard]] static TemplateGeometry buildTemplateGeometry(
        const TrackingBox& rgb_box,
        int rgb_image_width,
        int rgb_image_height,
        float template_resize_factor
    );
};

}  // namespace rkcam::ai::hit
