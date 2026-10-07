#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "rkcam/ai/image_loader.hpp"
#include "rkcam/ai/unaligned_hit/hit_input_processor.hpp"
#include "rkcam/ai/unaligned_hit/hit_types.hpp"
namespace fs = std::filesystem;

using rkcam::ai::Image;
using rkcam::ai::ImageLoader;
using rkcam::ai::hit::HiTDynamicTemplateInput;
using rkcam::ai::hit::HiTInitializationInput;
using rkcam::ai::hit::HiTInputProcessor;
using rkcam::ai::hit::HiTPatch;
using rkcam::ai::hit::HiTSearchInput;
using rkcam::ai::hit::Homography3x3;
using rkcam::ai::hit::TrackingBox;

namespace {

std::vector<fs::path> listJpgFiles(const fs::path& dir)
{
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        throw std::runtime_error("Image directory does not exist: " + dir.string());
    }
    std::vector<fs::path> files;
    for(const auto& entry : fs::directory_iterator(dir)) //fs::directory_iterator(dir)	创建一个目录迭代器，遍历 dir 下的所有条目
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        std::string ext = entry.path().extension().string();
        // 参数 1：输入区间起点

        // 参数 2：输入区间终点（左闭右开）

        // 参数 3：输出位置（这里用 ext.begin()，原地修改）

        // 参数 4：变换函数
        std::transform(
            ext.begin(),
            ext.end(),
            ext.begin(),
            [](unsigned char ch)
            {
                return static_cast<char>(std::tolower(ch));
            }
        );
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp") {
            files.push_back(entry.path());
        }
    }
    std::sort(
        files.begin(),
        files.end(),
        [](const fs::path& a, const fs::path& b)
        {
            return a.filename().string() < b.filename().string();
        }
    );
    if(files.empty())
    {
        throw std::runtime_error("No image files found in: " + dir.string());
    }
    return files;
}

std::vector<TrackingBox> loadBoxes(const fs::path& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open annotation file: " + path.string());
    }
    std::vector<TrackingBox> boxes;
    std::string line;
    int line_number = 0;
    while(std::getline(input, line))
    {
        ++line_number;
        for(char& ch : line)
        {
            if(ch == ',' || ch == '\t' || ch == ';')
            {
                ch = ' ';
            }
        } 
        std::istringstream iss(line);
        TrackingBox box{};
        if(!(iss >> box.x >> box.y >> box.w >> box.h))
        {
            //如果 line 里找不到任何一个"不是空格/回车/换行"的字符，说明这一行全是空白，跳过它。
            //找到第一个不是"\r\n"的字符，返回它的索引，没有找到返回-1，说明是空白行
            if(line.find_first_not_of("\r\n") == std::string::npos) //std::string::npos = -1;位置索引，表示没有找到"\r\n"
            {
                continue;
            }
            throw std::runtime_error(
                "Failed to parse [x,y,w,h] at " + path.string() +
                ":" + std::to_string(line_number) + " -> " + line
            );
        }
        if (!std::isfinite(box.x) ||
            !std::isfinite(box.y) ||
            !std::isfinite(box.w) ||
            !std::isfinite(box.h) ||
            box.w <= 0.0F ||
            box.h <= 0.0F) {
            throw std::runtime_error(
                "Invalid bbox at " + path.string() +
                ":" + std::to_string(line_number)
            );
        }
        boxes.push_back(box);
    }
    if(boxes.empty())
    {
        throw std::runtime_error("No valid boxes found in: " + path.string());
    }
    return boxes;
}



void printBox(const char* name, const TrackingBox& box)
{
    std::cout
        << name
        << " = [x=" << box.x
        << ", y=" << box.y
        << ", w=" << box.w
        << ", h=" << box.h
        << "]\n";
}

void printHomography(const Homography3x3& h)
{
    std::cout << "reference_h (native TIR -> RGB):\n";
    for (int row = 0; row < 3; ++row) {
        std::cout << "  ";
        for (int col = 0; col < 3; ++col) {
            std::cout
                << std::setw(12)
                << h.data[static_cast<std::size_t>(row * 3 + col)]
                << ' ';
        }
        std::cout << '\n';
    }
}



void verifyHomography(
    const Homography3x3& h,
    const TrackingBox& rgb_box,
    const TrackingBox& tir_box)
{
    const float sx = h.data[0];
    const float sy = h.data[4];
    const float tx = h.data[2];
    const float ty = h.data[5];

    const float mapped_x = sx * tir_box.x + tx;
    const float mapped_y = sy * tir_box.y + ty;
    const float mapped_w = sx * tir_box.w;
    const float mapped_h = sy * tir_box.h;

    const float max_error = std::max({
        std::abs(mapped_x - rgb_box.x),
        std::abs(mapped_y - rgb_box.y),
        std::abs(mapped_w - rgb_box.w),
        std::abs(mapped_h - rgb_box.h),
    });

    std::cout
        << "H0 bbox mapping max_abs_error = "
        << max_error << '\n';

    if (max_error > 1e-3F) {
        throw std::runtime_error("reference_h does not map initial TIR bbox to RGB bbox.");
    }
}

void validatePatch(
    const char* name,
    const HiTPatch& patch,
    int expected_width,
    int expected_height)
{
    if (patch.width != expected_width ||
        patch.height != expected_height ||
        patch.channels != 6) {
        throw std::runtime_error(
            std::string(name) + ": unexpected patch shape."
        );
    }

    const std::size_t expected_elements =
        static_cast<std::size_t>(expected_width) *
        static_cast<std::size_t>(expected_height) *
        6U;

    if (patch.raw_hwc.size() != expected_elements) {
        throw std::runtime_error(
            std::string(name) + ": raw_hwc element count mismatch."
        );
    }

    if (patch.tensor_nchw.size() != expected_elements) {
        throw std::runtime_error(
            std::string(name) + ": tensor_nchw element count mismatch."
        );
    }

    if (!std::isfinite(patch.resize_factor) || patch.resize_factor <= 0.0F) {
        throw std::runtime_error(
            std::string(name) + ": invalid resize_factor."
        );
    }

    for (float value : patch.tensor_nchw) {
        if (!std::isfinite(value)) {
            throw std::runtime_error(
                std::string(name) + ": tensor contains NaN/Inf."
            );
        }
    }
}

void printTensorStats(const char* name, const HiTPatch& patch)
{
    const std::size_t plane =
        static_cast<std::size_t>(patch.width) *
        static_cast<std::size_t>(patch.height);

    std::cout
        << name
        << ": shape=[1,6,"
        << patch.height
        << ","
        << patch.width
        << "]"
        << " resize_factor="
        << patch.resize_factor
        << '\n';

    for (int channel = 0; channel < 6; ++channel) {
        const std::size_t begin =
            static_cast<std::size_t>(channel) * plane;
        const std::size_t end = begin + plane;

        float min_value = std::numeric_limits<float>::infinity();
        float max_value = -std::numeric_limits<float>::infinity();
        double sum = 0.0;

        for (std::size_t i = begin; i < end; ++i) {
            const float value = patch.tensor_nchw[i];
            min_value = std::min(min_value, value);
            max_value = std::max(max_value, value);
            sum += static_cast<double>(value);
        }

        const double mean =
            sum / static_cast<double>(plane);

        std::cout
            << "  C" << channel
            << " min=" << min_value
            << " max=" << max_value
            << " mean=" << mean
            << '\n';
    }
}
void writePpm(const HiTPatch& patch, int channel_offset, const fs::path& path)
{
    //patch [H ,W, C]
    // 这个函数把 HiTPatch 中的 6 通道数据转成 PPM 格式的 RGB 图像并保存。
    // PPM（Portable PixMap）是一种简单的图像格式，P6 表示二进制 RGB。
    // 关键点：
    // channel_offset = 0 表示取 RGB 通道（前 3 个）
    // channel_offset = 3 表示取 TIR 通道（后 3 个）
    // 从 6 通道交错数据中抽取 3 个通道，写成一个 RGB PPM 文件
    // PPM P6 的文件结构
    // P6\n              ← 魔数，表示"二进制 RGB"
    // <width> <height>\n  ← 宽 高（用空格分隔）
    // 255\n             ← 最大像素值（8 位，就是 255）
    // <二进制 RGB 数据>   ← 每像素 3 字节：R, G, B
    if (channel_offset != 0 && channel_offset != 3) {
        throw std::invalid_argument("channel_offset must be 0 (RGB) or 3 (TIR).");
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Failed to create: " + path.string());
    }
    output
        << "P6\n"
        << patch.width
        << ' '
        << patch.height
        << "\n255\n";
    const std::size_t pixels = static_cast<std::size_t>(patch.width) * static_cast<std::size_t>(patch.height);
    std::vector<std::uint8_t> rgb(pixels *  3U);
    for(std::size_t i = 0; i < pixels; ++i)
    {
        const std::size_t src = i * 6U + static_cast<std::size_t>(channel_offset);
        const std::size_t dst = i * 3U;
        rgb[dst + 0U] = patch.raw_hwc[src + 0U];
        rgb[dst + 1U] = patch.raw_hwc[src + 1U];
        rgb[dst + 2U] = patch.raw_hwc[src + 2U];
    }
    output.write(
        reinterpret_cast<const char*>(rgb.data()),
        static_cast<std::streamsize>(rgb.size())
    );
    if (!output) {
        throw std::runtime_error("Failed while writing: " + path.string());
    }
}


void writeTensorF32(
    const HiTPatch& patch,
    const fs::path& path)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Failed to create: " + path.string());
    }

    output.write(
        reinterpret_cast<const char*>(patch.tensor_nchw.data()),
        static_cast<std::streamsize>(
            patch.tensor_nchw.size() * sizeof(float)
        )
    );

    if (!output) {
        throw std::runtime_error("Failed while writing: " + path.string());
    }
}

void dumpPatch(
    const std::string& prefix,
    const HiTPatch& patch,
    const fs::path& output_dir)
{
    writePpm(
        patch,
        0,
        output_dir / (prefix + "_rgb.ppm")
    );

    writePpm(
        patch,
        3,
        output_dir / (prefix + "_tir.ppm")
    );

    writeTensorF32(
        patch,
        output_dir / (prefix + "_nchw_f32.bin")
    );
}

void printUsage(const char* program)
{
    std::cout
        << "Usage:\n"
        << "  " << program
        << " <sequence_dir> [output_dir] [search_frame] [dynamic_frame]\n\n"
        << "Example:\n"
        << "  " << program
        << " apps/tools/rknn_deploy/test_video/1boygo"
        << " test_outputs/hit_input_processor 1 20\n\n"
        << "Meaning:\n"
        << "  search_frame=1:\n"
        << "    load frame 1 image, use RGB GT(frame 0) as a stand-in for PredRGB(0)\n"
        << "    when preparing its 256x256 search input.\n"
        << "  dynamic_frame=20:\n"
        << "    use raw frame 20 + RGB GT(frame 20) as a stand-in for PredRGB(20)\n"
        << "    to build a 128x128 Memory-M2 dynamic template.\n";
}
}//namespace

int main(int argc, char** argv)
{
    try {
        if (argc >= 2 &&
            (std::string(argv[1]) == "-h" ||
             std::string(argv[1]) == "--help")) {
            printUsage(argv[0]);
            return 0;
        }

        const fs::path sequence_dir =
            argc >= 2
                ? fs::path(argv[1])
                : fs::path("apps/tools/rknn_deploy/test_video/1boygo");

        const fs::path output_dir =
            argc >= 3
                ? fs::path(argv[2])
                : fs::path("test_outputs/hit_input_processor");

        const int search_frame =
            argc >= 4 ? std::stoi(argv[3]) : 1;

        const int dynamic_frame =
            argc >= 5 ? std::stoi(argv[4]) : 20;

        std::cout << std::fixed << std::setprecision(6);

        const fs::path visible_dir = sequence_dir / "visible";
        const fs::path infrared_dir = sequence_dir / "infrared";
        const fs::path visible_gt_path = sequence_dir / "visible.txt";
        const fs::path infrared_gt_path = sequence_dir / "infrared.txt";

        const auto rgb_paths = listJpgFiles(visible_dir);
        const auto tir_paths = listJpgFiles(infrared_dir);
        const auto rgb_boxes = loadBoxes(visible_gt_path);
        const auto tir_boxes = loadBoxes(infrared_gt_path);
      if (rgb_paths.size() != tir_paths.size()) {
            throw std::runtime_error(
                "RGB/TIR image count mismatch: " +
                std::to_string(rgb_paths.size()) +
                " vs " +
                std::to_string(tir_paths.size())
            );
        }

        const std::size_t frame_count = rgb_paths.size();

        if (rgb_boxes.size() < frame_count ||
            tir_boxes.size() < frame_count) {
            throw std::runtime_error(
                "Annotation count is smaller than image count."
            );
        }

        if (search_frame <= 0 ||
            static_cast<std::size_t>(search_frame) >= frame_count) {
            throw std::runtime_error(
                "search_frame must be in [1, frame_count-1]."
            );
        }

        if (dynamic_frame < 0 ||
            static_cast<std::size_t>(dynamic_frame) >= frame_count) {
            throw std::runtime_error(
                "dynamic_frame must be in [0, frame_count-1]."
            );
        }

        fs::create_directories(output_dir);

        std::cout << "============================================================\n";
        std::cout << "HiTInputProcessor standalone test\n";
        std::cout << "sequence_dir   : " << sequence_dir << '\n';
        std::cout << "frames         : " << frame_count << '\n';
        std::cout << "search_frame   : " << search_frame << '\n';
        std::cout << "dynamic_frame  : " << dynamic_frame << '\n';
        std::cout << "output_dir     : " << output_dir << '\n';
        std::cout << "============================================================\n\n";

        HiTInputProcessor processor;

        // ------------------------------------------------------------
        // 1. First-frame initialization
        // ------------------------------------------------------------
        std::cout << "[1] prepareInitialization()\n";

        const Image rgb0 = ImageLoader::loadRgb(rgb_paths[0].string());
        const Image tir0 = ImageLoader::loadRgb(tir_paths[0].string());

        const TrackingBox init_rgb_box = rgb_boxes[0];
        const TrackingBox init_tir_box = tir_boxes[0];

        std::cout
            << "RGB0: "
            << rgb_paths[0].filename()
            << " size="
            << rgb0.width << "x" << rgb0.height
            << "x" << rgb0.channels << '\n';

        std::cout
            << "TIR0: "
            << tir_paths[0].filename()
            << " size="
            << tir0.width << "x" << tir0.height
            << "x" << tir0.channels << '\n';

        printBox("init_rgb_box", init_rgb_box);
        printBox("init_tir_box", init_tir_box);

        const HiTInitializationInput initialization =
            processor.prepareInitialization(
                rgb0,
                tir0,
                init_rgb_box,
                init_tir_box
            );

        validatePatch(
            "static_template",
            initialization.static_template,
            128,
            128
        );

        printHomography(initialization.reference_h);
        verifyHomography(
            initialization.reference_h,
            init_rgb_box,
            init_tir_box
        );
       std::cout
            << "template_geometry = ["
            << initialization.template_geometry.values[0] << ", "
            << initialization.template_geometry.values[1] << ", "
            << initialization.template_geometry.values[2] << ", "
            << initialization.template_geometry.values[3] << "]\n";

        for (float value : initialization.template_geometry.values) {
            if (!std::isfinite(value)) {
                throw std::runtime_error(
                    "template_geometry contains NaN/Inf."
                );
            }
        }

        printTensorStats(
            "static_template",
            initialization.static_template
        );

        dumpPatch(
            "static_template",
            initialization.static_template,
            output_dir
        );
        std::cout << "[PASS] initialization\n\n";

        // ------------------------------------------------------------
        // 2. Search input
        // ------------------------------------------------------------
        //
        // Actual tracker:
        //   frame t image + PredRGB(t-1)
        //
        // This standalone preprocessor test has no model yet, so we use:
        //   frame t image + GT_RGB(t-1)
        //
        // only as a known-valid stand-in for the tracking state.
        // ------------------------------------------------------------
        std::cout << "[2] prepareSearch()\n";

        const Image rgb_search_frame =
            ImageLoader::loadRgb(
                rgb_paths[static_cast<std::size_t>(search_frame)].string()
            );

        const Image tir_search_frame =
            ImageLoader::loadRgb(
                tir_paths[static_cast<std::size_t>(search_frame)].string()
            );

        const TrackingBox search_reference_state =
            rgb_boxes[static_cast<std::size_t>(search_frame - 1)];

        std::cout
            << "search RGB image: "
            << rgb_paths[static_cast<std::size_t>(search_frame)].filename()
            << '\n';

        std::cout
            << "search TIR image: "
            << tir_paths[static_cast<std::size_t>(search_frame)].filename()
            << '\n';
        printBox(
            "search_reference_state(GT used as Pred stand-in)",
            search_reference_state
        );

        const HiTSearchInput search =
            processor.prepareSearch(
                rgb_search_frame,
                tir_search_frame,
                search_reference_state,
                initialization.reference_h
            );

        validatePatch(
            "search",
            search.search,
            256,
            256
        );
      if (!std::isfinite(search.resize_factor_rgb) ||
            search.resize_factor_rgb <= 0.0F ||
            !std::isfinite(search.resize_factor_ir) ||
            search.resize_factor_ir <= 0.0F) {
            throw std::runtime_error("Invalid search resize factor.");
        }

        std::cout
            << "search.resize_factor_rgb = "
            << search.resize_factor_rgb << '\n';

        std::cout
            << "search.resize_factor_ir  = "
            << search.resize_factor_ir << '\n';

        printTensorStats("search", search.search);
        dumpPatch("search", search.search, output_dir);

        std::cout << "[PASS] search\n\n";

        // ------------------------------------------------------------
        // 3. Dynamic template
        // ------------------------------------------------------------
        //
        // Actual M2:
        //   raw frame k + PredRGB(k) + fixed H0
        //
        // Standalone test:
        //   raw frame k + GT_RGB(k) + fixed H0
        // ------------------------------------------------------------
        std::cout << "[3] prepareDynamicTemplate()\n";
        const Image rgb_dynamic_frame =
            ImageLoader::loadRgb(
                rgb_paths[static_cast<std::size_t>(dynamic_frame)].string()
            );

        const Image tir_dynamic_frame =
            ImageLoader::loadRgb(
                tir_paths[static_cast<std::size_t>(dynamic_frame)].string()
            );

        const TrackingBox dynamic_rgb_state =
            rgb_boxes[static_cast<std::size_t>(dynamic_frame)];

        printBox(
            "dynamic_rgb_state(GT used as Pred stand-in)",
            dynamic_rgb_state
        );

        const HiTDynamicTemplateInput dynamic =
            processor.prepareDynamicTemplate(
                rgb_dynamic_frame,
                tir_dynamic_frame,
                dynamic_rgb_state,
                initialization.reference_h,
                dynamic_frame
            );

        validatePatch(
            "dynamic_template",
            dynamic.dynamic_template,
            128,
            128
        );

        if (dynamic.source_frame != dynamic_frame) {
            throw std::runtime_error(
                "Dynamic template source_frame mismatch."
            );
        }

        printTensorStats(
            "dynamic_template",
            dynamic.dynamic_template
        );

        dumpPatch(
            "dynamic_template",
            dynamic.dynamic_template,
            output_dir
        );

        std::cout
            << "dynamic.source_frame = "
            << dynamic.source_frame << '\n';
      std::cout << "[PASS] dynamic template\n\n";

        std::cout << "============================================================\n";
        std::cout << "ALL HiTInputProcessor tests PASSED\n";
        std::cout << "Generated visual checks:\n";
        std::cout << "  " << output_dir / "static_template_rgb.ppm" << '\n';
        std::cout << "  " << output_dir / "static_template_tir.ppm" << '\n';
        std::cout << "  " << output_dir / "search_rgb.ppm" << '\n';
        std::cout << "  " << output_dir / "search_tir.ppm" << '\n';
        std::cout << "  " << output_dir / "dynamic_template_rgb.ppm" << '\n';
        std::cout << "  " << output_dir / "dynamic_template_tir.ppm" << '\n';
        std::cout << "Raw normalized NCHW float32 tensors are also dumped as *.bin\n";
        std::cout << "============================================================\n";

        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "\n[FAILED] hit_input_processor_test: "
            << error.what()
            << '\n';
        return 1;
    }
}