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

#include <onnxruntime_cxx_api.h>

#include "rkcam/ai/image_loader.hpp"
#include "rkcam/ai/types.hpp"
#include "rkcam/ai/unaligned_hit/hit_input_processor.hpp"
#include "rkcam/ai/unaligned_hit/hit_types.hpp"


namespace fs = std::filesystem;

using rkcam::ai::FloatTensor;
using rkcam::ai::Image;
using rkcam::ai::ImageLoader;
using rkcam::ai::TensorLayout;

using rkcam::ai::hit::HiTInitializationInput;
using rkcam::ai::hit::HiTInputProcessor;
using rkcam::ai::hit::HiTPatch;
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

std::vector<fs::path> listImageFiles(const fs::path& dir)
{
    std::vector<fs::path> files;
    if(!fs::exists(dir) || !fs::is_directory(dir))
    {
        throw std::runtime_error(
            "Image directory does not exist: "
            + dir.string()
        );
    }
    for(auto& entry : fs::directory_iterator(dir))
    {
        if(!entry.is_regular_file())
        {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c)
            {
                return static_cast<char>(std::tolower(c));
            }
        );

        if (
            ext == ".jpg"
            ||
            ext == ".jpeg"
            ||
            ext == ".png"
            ||
            ext == ".bmp"
        ) {
            files.push_back(
                entry.path()
            );
        }
        
    }

    std::sort(
        files.begin(),
        files.end(),
        [](
            const fs::path& a,
            const fs::path& b
        ) {
            return
                a.filename().string()
                <
                b.filename().string();
        }
    );


    return files;
}

std::vector<TrackingBox> loadBoxes(const fs::path& path)
{
    std::ifstream input(path);
    if(!input)
    {
        throw std::runtime_error(
            "Failed to open annotation: "
            +
            path.string()
        );
    }
    std::vector<TrackingBox> boxes;
    std::string line;
    while(getline(input, line))
    {
        for (
            char& ch :
            line
        ) {
            if (
                ch == ','
                ||
                ch == '\t'
                ||
                ch == ';'
            ) {
                ch = ' ';
            }
        }
        if(line.find_first_not_of("\r\n") == std::string::npos)
        {
            continue;
        }
        std::istringstream iss(line);
        TrackingBox box{};
        if (
            !(iss
              >> box.x
              >> box.y
              >> box.w
              >> box.h)
        ) {
            throw std::runtime_error(
                "Failed to parse bbox"
            );
        }
        boxes.push_back(
            box
        );
    }
    return boxes;
}



FloatTensor patchToTensor(
    const HiTPatch& patch
) {
    FloatTensor tensor;

    tensor.shape.dims = {
        1,
        6,
        patch.height,
        patch.width,
    };

    tensor.layout =
        TensorLayout::NCHW;

    tensor.data =
        patch.tensor_nchw;

    if (!tensor.valid()) {
        throw std::runtime_error(
            "Invalid patch tensor"
        );
    }

    return tensor;
}


void printShape(
    const std::vector<int64_t>& shape
) {
    std::cout << "[";

    for (
        std::size_t i = 0;
        i < shape.size();
        ++i
    ) {
        if (i != 0) {
            std::cout << ",";
        }

        std::cout
            << shape[i];
    }

    std::cout << "]";
}

void printTensorStats(
    const char* name,
    const FloatTensor& tensor
) {
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


    std::cout
        << name
        << " count="
        << tensor.data.size()
        << " min="
        << min_value
        << " max="
        << max_value
        << " mean="
        << (
            sum
            /
            static_cast<double>(
                tensor.data.size()
            )
        )
        << "\n";
}
void dumpBinary(
    const std::string& path,
    const std::vector<float>& data
)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error(
            "Cannot create: "
            + path
        );
    }
    output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * sizeof(float)));
}
void dumpText(
    const std::string& path,
    const std::vector<float>& data
)
{
    std::ofstream output(path);
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
        +
        "_"
        +
        name
        +
        ".bin",
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
        << " <model.onnx>"
        << " <sequence_dir>"
        << " [search_frame]"
        << " [dump_prefix]\n";
}

}

int main(int argc, char** argv)
{
    try{
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
        const std::string model_path = argv[1];
        const fs::path sequence_dir = argv[2];
        const int search_frame = argc >= 4 ? std::stoi(argv[3]) : 1;
        const std::string dump_prefix = argc >= 5 ? argv[4] : "onnx_hit";
        std::cout << std::fixed << std::setprecision(9);
        // ========================================================
        // 1. ONNX model
        // ========================================================
        std::cout
            << "============================================================\n"
            << "ONNX RGBT HiT RAW TEST\n"
            << "============================================================\n";

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_rgbt_hit");
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        const auto init_begin = Clock::now();
        Ort::Session session(env, model_path.c_str(), options);
        const auto init_end = Clock::now();
        Ort::AllocatorWithDefaultOptions allocator;
        if(session.GetInputCount() != 4)
        {
            throw std::runtime_error(
                "Expected 4 ONNX inputs"
            );
        }
        if(session.GetOutputCount() != 1)
        {
            throw std::runtime_error(
                "Expected 1 ONNX output"
            );
        }
        const std::array<std::string, 4> expected_names{
            "template",
            "search",
            "dynamic_template",
            "template_geometry",
        };
        std::vector<std::string> input_names;
        for (
            std::size_t i = 0;
            i < 4;
            ++i
        ) 
        {
            auto name_ptr = session.GetInputNameAllocated(i, allocator);
            input_names.emplace_back(name_ptr.get());
            const auto type_info = session.GetInputTypeInfo(i);
            const auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
            const auto shape = tensor_info.GetShape();
            std::cout
                << "input["
                << i
                << "] name="
                << input_names.back()
                << " shape=";
            printShape(
                shape
            );
            std::cout
                << "\n";
            if (
                input_names.back()
                !=
                expected_names[i]
            ) {
                throw std::runtime_error(
                    "ONNX input order mismatch"
                );
            }
        }
        auto output_name_ptr = session.GetOutputNameAllocated(0, allocator);
        const std::string output_name = output_name_ptr.get();
        if (
            output_name
            !=
            "pred_boxes"
        ) {
            throw std::runtime_error(
                "Expected output pred_boxes"
            );
        }


        std::cout
            << "init time: "
            << elapsedMs(
                init_begin,
                init_end
            )
            << " ms\n\n";


        // ========================================================
        // 2. HiT preprocessing
        // ========================================================
        const auto rgb_paths = listImageFiles(sequence_dir/"visible");
        const auto tir_paths = listImageFiles(sequence_dir/"infrared");

        const auto rgb_boxes = loadBoxes(sequence_dir/"visible.txt");
        const auto tir_boxes = loadBoxes(sequence_dir/"infrared.txt");

        HiTInputProcessor processor;
        const Image rgb0 = ImageLoader::loadRgb(rgb_paths[0].string());
        const Image tir0 = ImageLoader::loadRgb(tir_paths[0].string());

        HiTInitializationInput init = processor.prepareInitialization(
            rgb0,
            tir0,
            rgb_boxes[0],
            tir_boxes[0]
        );

        const Image rgb_search = ImageLoader::loadRgb(
            rgb_paths[static_cast<std::size_t>(search_frame)].string()
        );
        const Image tir_search = ImageLoader::loadRgb(
            tir_paths[static_cast<std::size_t>(search_frame)].string()
        );

        const TrackingBox search_state = rgb_boxes[static_cast<std::size_t>(search_frame - 1)];
        
        const HiTSearchInput search = processor.prepareSearch(
            rgb_search,
            tir_search,
            search_state,
            init.reference_h
        );

        // ========================================================
        // 3. Create exactly the same four tensors
        // ========================================================
        std::array<FloatTensor, 4> inputs;

        inputs[0] = patchToTensor(init.static_template);
        inputs[1] = patchToTensor(search.search);
        //Td = T0
        inputs[2] = patchToTensor(init.static_template);
        inputs[3].shape.dims ={1, 4};
        inputs[3].layout = TensorLayout::NCHW;
        inputs[3].data.assign(
            init.template_geometry.values.begin(),
            init.template_geometry.values.end()
        );

        printTensorStats(
            "template",
            inputs[0]
        );

        printTensorStats(
            "search",
            inputs[1]
        );

        printTensorStats(
            "dynamic_template",
            inputs[2]
        );

        printTensorStats(
            "template_geometry",
            inputs[3]
        );


        dumpTensor(
            dump_prefix,
            "template",
            inputs[0]
        );

        dumpTensor(
            dump_prefix,
            "search",
            inputs[1]
        );

        dumpTensor(
            dump_prefix,
            "dynamic_template",
            inputs[2]
        );

        dumpTensor(
            dump_prefix,
            "template_geometry",
            inputs[3]
        );


        // ========================================================
        // 4. Create ORT tensors
        // ========================================================
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator,
            OrtMemTypeDefault
        );
        std::vector<Ort::Value> ort_inputs;
        ort_inputs.reserve(4);

        for(std::size_t i = 0; i < 4; ++i)
        {
            ort_inputs.emplace_back(Ort::Value::CreateTensor<float>(
                memory_info,
                inputs[i].data.data(),
                inputs[i].data.size(),
                inputs[i].shape.dims.data(),
                inputs[i].shape.dims.size()
            ));
        }

        std::vector<const char*> input_name_ptrs; //这里是因为session.Run输入需要char*类型的，而不是string的，这里进行类型转换
        for (
            const auto& name :
            input_names
        ) {
            input_name_ptrs.push_back(
                name.c_str()
            );
        }
        const char* output_name_raw = output_name.c_str();
        // ========================================================
        // 5. ONNX inference
        // ========================================================
        std::cout << "\n[3] ONNX inference\n";
        const auto infer_begin = Clock::now();
        // std::vector<Ort::Value> Run(
        //     const RunOptions& run_options,
        //     const char* const* input_names,      // C 风格字符串数组
        //     const Ort::Value* input_values,      // Ort::Value 数组
        //     size_t input_count,                  // 元素个数
        //     const char* const* output_names,
        //     size_t output_count
        // );
        auto outputs = session.Run(
            Ort::RunOptions{nullptr},
            input_name_ptrs.data(),
            ort_inputs.data(),
            ort_inputs.size(),
            &output_name_raw,
            1
        );
        const auto infer_end = Clock::now();

        if(outputs.size() != 1)
        {
            throw std::runtime_error(
                "Unexpected ONNX output count"
            );
        }

        const auto info = outputs[0].GetTensorTypeAndShapeInfo();
        if(info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        {
            throw std::runtime_error(
                "pred_boxes is not float32"
            );
        }
        if (
            info.GetElementCount()
            !=
            4
        ) {
            throw std::runtime_error(
                "pred_boxes must contain 4 values"
            );
        }
        const float* data = outputs[0].GetTensorData<float>();
        std::vector<float> boxes{
            data[0],
            data[1],
            data[2],
            data[3],
        };

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
            << boxes[0]
            << "\n"
            << "cy = "
            << boxes[1]
            << "\n"
            << "w  = "
            << boxes[2]
            << "\n"
            << "h  = "
            << boxes[3]
            << "\n";


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
            << "ONNX HiT RAW TEST PASSED\n"
            << "============================================================\n";


        return 0;

    }
    catch (
        const Ort::Exception& error
    ) {
        std::cerr
            << "\n[ORT ERROR] "
            << error.what()
            << "\n";

        return 11;
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
}