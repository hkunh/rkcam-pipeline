#include <algorithm>
#include <array>
#include <chrono>
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
#include "rkcam/ai/types.hpp"
#include "rkcam/ai/unaligned_hit/hit_input_processor.hpp"
#include "rkcam/ai/unaligned_hit/hit_model_runner.hpp"
#include "rkcam/ai/unaligned_hit/hit_types.hpp"

namespace fs = std::filesystem;

using rkcam::ai::FloatTensor;
using rkcam::ai::Image;
using rkcam::ai::ImageLoader;
using rkcam::ai::TensorLayout;

using rkcam::ai::hit::HiTInitializationInput;
using rkcam::ai::hit::HiTInputProcessor;
using rkcam::ai::hit::HiTModelInput;
using rkcam::ai::hit::HiTModelRunner;
using rkcam::ai::hit::HiTPatch;
using rkcam::ai::hit::HiTRawOutput;
using rkcam::ai::hit::HiTSearchInput;
using rkcam::ai::hit::TrackingBox;


namespace{
using Clock = std::chrono::steady_clock;

double elapsedMs(
    const Clock::time_point& begin,
    const Clock::time_point& end
)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}


std::vector<fs::path> listImageFiles(
    const fs::path& dir
)
{
    if(!fs::exists(dir) || !fs::is_directory(dir))
    {
        throw std::runtime_error(
            "Image directory does not exist: "
            + dir.string()
        );
    }

    std::vector<fs::path> files;
    for(const auto& entry : fs::directory_iterator(dir))
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char ch){
                return static_cast<char>(std::tolower(ch));
            }
        );
        // std::transform(
        //     ext.begin(),
        //     ext.end(),
        //     ext.begin(),
        //     [](unsigned char ch){
        //         return static_cast<char>(
        //             std::tolower(ch)
        //         );
        //     }
        // );
        // std::transform(
        //     ext.begin(),
        //     ext.end(),
        //     ext.begin(),
        //     [](unsigned char ch) {
        //         return static_cast<char>(
        //             std::tolower(ch)
        //         );
        //     }
        // );
        if (ext == ".jpg" ||
            ext == ".jpeg" ||
            ext == ".png" ||
            ext == ".bmp") {
            files.push_back(
                entry.path()
            );
        }
    }
    std::sort(files.begin(), files.end(), 
        [](const fs::path& a, const fs::path& b){
            return a.filename().string() < b.filename().string();
        }
    );
    if (files.empty()) {
        throw std::runtime_error(
            "No image files found in: "
            + dir.string()
        );
    }
    return files;

}


std::vector<TrackingBox> loadBoxes(const fs::path& path)
{
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "Failed to open annotation: "
            + path.string()
        );
    }
    std::vector<TrackingBox> boxes;
    std::string line;
    int line_number = 0;
    while(getline(input, line))
    {
        ++line_number;
        for(char& ch : line)
        {
            if(ch == ',' || ch == '\t' || ch == ';')
            {
                ch = ' ';
            } 
        }
        if(line.find_first_not_of("\r\n") == std::string::npos)
        {
            continue;
        }
        std::istringstream iss(line);
        TrackingBox box{};
        if(!(iss >> box.x >> box.y >> box.w >> box.h))
        {
            throw std::runtime_error(
                "Failed to parse bbox at "
                + path.string()
                + ":"
                + std::to_string(
                    line_number
                )
            );
        }

        if (
            !std::isfinite(box.x)
            ||
            !std::isfinite(box.y)
            ||
            !std::isfinite(box.w)
            ||
            !std::isfinite(box.h)
            ||
            box.w <= 0.0F
            ||
            box.h <= 0.0F
        ) {
            throw std::runtime_error(
                "Invalid bbox at "
                + path.string()
                + ":"
                + std::to_string(
                    line_number
                )
            );
        }
        boxes.push_back(box);
    }

    if (boxes.empty()) {
        throw std::runtime_error(
            "No bbox found in: "
            + path.string()
        );
    }

    return boxes;

}
FloatTensor patchToTensor(const HiTPatch& patch)
{
    if (
        patch.width <= 0
        ||
        patch.height <= 0
        ||
        patch.channels != 6
    ) {
        throw std::runtime_error(
            "Invalid HiTPatch"
        );
    }
    const std::size_t expected =
        static_cast<std::size_t>(
            patch.width
        )
        *
        static_cast<std::size_t>(
            patch.height
        )
        *
        6U;
    if (
        patch.tensor_nchw.size()
        !=
        expected
    ) {
        throw std::runtime_error(
            "HiTPatch tensor size mismatch"
        );
    }
    FloatTensor tensor;
    tensor.shape.dims = {1, 6, patch.height, patch.width};
    tensor.layout = TensorLayout::NCHW;

    tensor.data = patch.tensor_nchw;

    return tensor;

}


void printShape(
    const rkcam::ai::TensorShape& shape
) {
    std::cout << "[";

    for (
        std::size_t i = 0;
        i < shape.dims.size();
        ++i
    ) {
        if (i != 0) {
            std::cout << ",";
        }

        std::cout
            << shape.dims[i];
    }

    std::cout << "]";
}


const char* layoutToString(
    TensorLayout layout
) {
    switch (layout) {

        case TensorLayout::NCHW:
            return "NCHW";

        case TensorLayout::NHWC:
            return "NHWC";

        case TensorLayout::NC1HWC2:
            return "NC1HWC2";

        default:
            return "Unknown";
    }
}


void printTensorStats(
    const char* name,
    const FloatTensor& tensor
) {
    if (!tensor.valid()) {
        throw std::runtime_error(
            std::string(name)
            + " tensor invalid"
        );
    }

    float min_value =
        std::numeric_limits<
            float
        >::infinity();

    float max_value =
        -std::numeric_limits<
            float
        >::infinity();

    double sum = 0.0;

    for (
        float value :
        tensor.data
    ) {
        if (!std::isfinite(value)) {
            throw std::runtime_error(
                std::string(name)
                + " contains NaN/Inf"
            );
        }

        min_value =
            std::min(
                min_value,
                value
            );

        max_value =
            std::max(
                max_value,
                value
            );

        sum += value;
    }

    const double mean =
        sum
        /
        static_cast<double>(
            tensor.data.size()
        );

    std::cout
        << name
        << " shape=";

    printShape(
        tensor.shape
    );

    std::cout
        << " layout="
        << layoutToString(
            tensor.layout
        )
        << " count="
        << tensor.data.size()
        << " min="
        << min_value
        << " max="
        << max_value
        << " mean="
        << mean
        << "\n";
}

void dumpBinary(const std::string& path, const std::vector<float>& data)
{
    std::ofstream output(path,  std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "Cannot create: "
            + path
        );
    }
    if(!data.empty())
    {
        output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
    }
}

void dumpText(const std::string& path, const std::vector<float>& data)
{
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error(
            "Cannot create: "
            + path
        );
    }
    output << std::scientific << std::setprecision(10);
    for(std::size_t i = 0; i < data.size(); ++i)
    {
        output << i << " " << data[i] << "\n";
    }
}



void dumpTensor(
    const std::string& prefix,
    const char* name,
    const FloatTensor& tensor
) {
    dumpBinary(
        prefix
        + "_"
        + name
        + ".bin",
        tensor.data
    );
}


void printUsage(
    const char* program
) {
    std::cout
        << "Usage:\n"
        << "  "
        << program
        << " <model.rknn>"
        << " <sequence_dir>"
        << " [search_frame]"
        << " [dump_prefix]\n\n"

        << "Example:\n"
        << "  "
        << program
        << " ./model/hit.rknn"
        << " ./test_video/1boygo"
        << " 1"
        << " rknn_hit\n";
}
FloatTensor nchwToNhwc(const FloatTensor& src)
{
    if (src.layout != TensorLayout::NCHW ||
        src.shape.dims.size() != 4)
    {
        throw std::runtime_error("nchwToNhwc expects rank-4 NCHW");
    }

    const int64_t n = src.shape.dims[0];
    const int64_t c = src.shape.dims[1];
    const int64_t h = src.shape.dims[2];
    const int64_t w = src.shape.dims[3];

    FloatTensor dst;

    dst.shape.dims = {
        n,
        h,
        w,
        c
    };

    dst.layout = TensorLayout::NHWC;

    dst.data.resize(src.data.size());

    for (int64_t ni = 0; ni < n; ++ni)
    {
        for (int64_t yi = 0; yi < h; ++yi)
        {
            for (int64_t xi = 0; xi < w; ++xi)
            {
                for (int64_t ci = 0; ci < c; ++ci)
                {
                    const std::size_t src_index =
                        ((ni * c + ci) * h + yi) * w + xi;

                    const std::size_t dst_index =
                        ((ni * h + yi) * w + xi) * c + ci;

                    dst.data[dst_index] =
                        src.data[src_index];
                }
            }
        }
    }

    return dst;
}
}

int main(int argc, char** argv)
{
    try {

        if (
            argc < 3
            ||
            argc > 5
        ) {
            printUsage(
                argv[0]
            );

            return 1;
        }

        const std::string model_path =
            argv[1];

        const fs::path sequence_dir =
            argv[2];

        const int search_frame =
            argc >= 4
                ?
                std::stoi(argv[3])
                :
                1;

        const std::string dump_prefix =
            argc >= 5
                ?
                argv[4]
                :
                "rknn_hit";


        std::cout
            << std::fixed
            << std::setprecision(9);


        // ========================================================
        // 1. RKNN model
        // ========================================================

        std::cout
            << "============================================================\n"
            << "RKNN RGBT HiT RAW TEST\n"
            << "============================================================\n"
            << "model        : "
            << model_path
            << "\n"
            << "sequence_dir : "
            << sequence_dir
            << "\n"
            << "search_frame : "
            << search_frame
            << "\n\n";


        HiTModelRunner runner(
            model_path
        );


        const auto init_begin =
            Clock::now();


        if (!runner.init()) {
            throw std::runtime_error(
                "HiTModelRunner init failed: "
                +
                runner.lastError()
            );
        }


        const auto init_end =
            Clock::now();


        std::cout
            << "[1] RKNN model\n"
            << "init time: "
            << elapsedMs(
                init_begin,
                init_end
            )
            << " ms\n";


        for (
            std::size_t i = 0;
            i < runner.engine().inputCount();
            ++i
        ) {
            const auto& attr =
                runner.engine()
                    .inputAttr(i);

            std::cout
                << "input["
                << i
                << "] name="
                << attr.name
                << " shape=";

            printShape(
                attr.shape
            );

            std::cout
                << " layout="
                << layoutToString(
                    attr.layout
                )
                << "\n";
        }


        const auto& output_attr =
            runner.engine()
                .outputAttr(0);

        std::cout
            << "output name="
            << output_attr.name
            << " shape=";

        printShape(
            output_attr.shape
        );

        std::cout << "\n\n";


        // ========================================================
        // 2. Dataset
        // ========================================================

        const auto rgb_paths =
            listImageFiles(
                sequence_dir
                /
                "visible"
            );

        const auto tir_paths =
            listImageFiles(
                sequence_dir
                /
                "infrared"
            );

        const auto rgb_boxes =
            loadBoxes(
                sequence_dir
                /
                "visible.txt"
            );

        const auto tir_boxes =
            loadBoxes(
                sequence_dir
                /
                "infrared.txt"
            );


        if (
            rgb_paths.size()
            !=
            tir_paths.size()
        ) {
            throw std::runtime_error(
                "RGB/TIR frame count mismatch"
            );
        }


        if (
            search_frame <= 0
            ||
            static_cast<std::size_t>(
                search_frame
            )
            >=
            rgb_paths.size()
        ) {
            throw std::runtime_error(
                "search_frame out of range"
            );
        }


        // ========================================================
        // 3. HiT preprocessing
        // ========================================================

        std::cout
            << "[2] HiT preprocessing\n";


        HiTInputProcessor processor;


        const Image rgb0 =
            ImageLoader::loadRgb(
                rgb_paths[0].string()
            );

        const Image tir0 =
            ImageLoader::loadRgb(
                tir_paths[0].string()
            );


        const HiTInitializationInput init =
            processor.prepareInitialization(
                rgb0,
                tir0,
                rgb_boxes[0],
                tir_boxes[0]
            );


        const Image rgb_search =
            ImageLoader::loadRgb(
                rgb_paths[
                    static_cast<
                        std::size_t
                    >(
                        search_frame
                    )
                ].string()
            );


        const Image tir_search =
            ImageLoader::loadRgb(
                tir_paths[
                    static_cast<
                        std::size_t
                    >(
                        search_frame
                    )
                ].string()
            );


        const TrackingBox search_state =
            rgb_boxes[
                static_cast<
                    std::size_t
                >(
                    search_frame - 1
                )
            ];


        const HiTSearchInput search =
            processor.prepareSearch(
                rgb_search,
                tir_search,
                search_state,
                init.reference_h
            );


        HiTModelInput model_input;


        model_input.staticTemplate() =
            nchwToNhwc(
                patchToTensor(init.static_template)
            );

        model_input.search() =
            nchwToNhwc(
                patchToTensor(search.search)
            );


        // First online tracking inference:
        //
        // Td = T0
        //
        model_input.dynamicTemplate() =
            nchwToNhwc(
                patchToTensor(init.static_template)
            );


        FloatTensor geometry;

        geometry.shape.dims = {
            1,
            4
        };

        geometry.layout =
            runner.engine()
                .inputAttr(3)
                .layout;

        geometry.data.assign(
            init.template_geometry
                .values
                .begin(),
            init.template_geometry
                .values
                .end()
        );


        model_input.templateGeometry() =
            std::move(
                geometry
            );


        printTensorStats(
            "template",
            model_input.staticTemplate()
        );

        printTensorStats(
            "search",
            model_input.search()
        );

        printTensorStats(
            "dynamic_template",
            model_input.dynamicTemplate()
        );

        printTensorStats(
            "template_geometry",
            model_input.templateGeometry()
        );


        // Dump four real model inputs.
        dumpTensor(
            dump_prefix,
            "template",
            model_input.staticTemplate()
        );

        dumpTensor(
            dump_prefix,
            "search",
            model_input.search()
        );

        dumpTensor(
            dump_prefix,
            "dynamic_template",
            model_input.dynamicTemplate()
        );

        dumpTensor(
            dump_prefix,
            "template_geometry",
            model_input.templateGeometry()
        );


        // ========================================================
        // 4. RKNN raw inference
        // ========================================================

        std::cout
            << "\n[3] RKNN inference\n";


        HiTRawOutput result;


        const auto infer_begin =
            Clock::now();


        if (
            !runner.infer(
                model_input,
                result
            )
        ) {
            throw std::runtime_error(
                "RKNN inference failed: "
                +
                runner.lastError()
            );
        }


        const auto infer_end =
            Clock::now();


        std::cout
            << "inference time: "
            << elapsedMs(
                infer_begin,
                infer_end
            )
            << " ms\n";


        std::cout
            << "\npred_boxes [cx, cy, w, h]\n"
            << "cx = "
            << result.pred_boxes[0]
            << "\n"
            << "cy = "
            << result.pred_boxes[1]
            << "\n"
            << "w  = "
            << result.pred_boxes[2]
            << "\n"
            << "h  = "
            << result.pred_boxes[3]
            << "\n";


        std::vector<float> boxes(
            result.pred_boxes.begin(),
            result.pred_boxes.end()
        );


        dumpBinary(
            dump_prefix
            +
            "_pred_boxes.bin",
            boxes
        );


        dumpText(
            dump_prefix
            +
            "_pred_boxes.txt",
            boxes
        );


        std::cout
            << "\n============================================================\n"
            << "RKNN HiT RAW TEST PASSED\n"
            << "============================================================\n";


        return 0;
    }
    catch (
        const std::exception& error
    ) {
        std::cerr
            << "\n[FAILED] "
            << error.what()
            << "\n";

        return 1;
    }
    return 0;
}