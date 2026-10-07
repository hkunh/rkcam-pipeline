#include "rkcam/ai/image_loader.hpp"
#include "rkcam/ai/rgbt_preprocessor.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rkcam::ai::TensorAttr;
using rkcam::ai::TensorDataType;
using rkcam::ai::TensorLayout;
using rkcam::ai::TensorShape;

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void printShape(const std::vector<int64_t>& shape)
{
    std::cout << "[";
    for (std::size_t i = 0; i < shape.size(); ++i) {
        std::cout << shape[i];
        if (i + 1 < shape.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "]";
}

void printShape(const TensorShape& shape)
{
    printShape(shape.dims);
}

struct OutputIndices {
    std::size_t logits = static_cast<std::size_t>(-1);
    std::size_t boxes = static_cast<std::size_t>(-1);
};

bool contains(const std::string& text, const std::string& target)
{
    return text.find(target) != std::string::npos;
}

OutputIndices findOutputs(const std::vector<std::string>& names,
                          const std::vector<std::vector<int64_t>>& shapes)
{
    if (names.size() != shapes.size()) {
        throw std::runtime_error("ONNX output name/shape count mismatch");
    }

    OutputIndices result;

    // 1. Prefer output names.
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (contains(names[i], "pred_logits")) {
            result.logits = i;
        }
        if (contains(names[i], "pred_boxes")) {
            result.boxes = i;
        }
    }

    // 2. Fallback to shape [1,Q,C] and [1,Q,4].
    if (result.logits == static_cast<std::size_t>(-1) ||
        result.boxes == static_cast<std::size_t>(-1)) {
        for (std::size_t i = 0; i < shapes.size(); ++i) {
            const auto& shape = shapes[i];
            if (shape.size() != 3) {
                continue;
            }
            if (shape[2] == 4 &&
                result.boxes == static_cast<std::size_t>(-1)) {
                result.boxes = i;
            } else if (shape[2] == 3 &&
                       result.logits == static_cast<std::size_t>(-1)) {
                result.logits = i;
            }
        }
    }

    if (result.logits == static_cast<std::size_t>(-1) ||
        result.boxes == static_cast<std::size_t>(-1)) {
        throw std::runtime_error("Cannot identify pred_logits / pred_boxes outputs");
    }

    return result;
}

void printStats(const char* name, const std::vector<float>& data)
{
    if (data.empty()) {
        std::cout << name << ": empty\n";
        return;
    }

    float min_value = std::numeric_limits<float>::infinity();
    float max_value = -std::numeric_limits<float>::infinity();
    long double sum = 0.0L;

    for (const float value : data) {
        min_value = std::min(min_value, value);
        max_value = std::max(max_value, value);
        sum += value;
    }

    const double mean = static_cast<double>(sum / data.size());

    std::cout << name << " stats:\n";
    std::cout << "  count = " << data.size() << "\n";
    std::cout << "  min   = " << std::setprecision(10) << min_value << "\n";
    std::cout << "  max   = " << std::setprecision(10) << max_value << "\n";
    std::cout << "  mean  = " << std::setprecision(10) << mean << "\n";
}

void printFirstValues(const char* name,
                      const std::vector<float>& data,
                      std::size_t count = 20)
{
    const std::size_t n = std::min(count, data.size());
    std::cout << "\n" << name << " first " << n << " values:\n";
    std::cout << std::setprecision(10);
    for (std::size_t i = 0; i < n; ++i) {
        std::cout << "  [" << i << "] = " << data[i] << "\n";
    }
}

void dumpBinary(const std::string& path, const std::vector<float>& data)
{
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs) {
        throw std::runtime_error("Cannot open output file: " + path);
    }

    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size() * sizeof(float)));
    }

    if (!ofs.good()) {
        throw std::runtime_error("Failed to write output file: " + path);
    }
}

void dumpText(const std::string& path, const std::vector<float>& data)
{
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::runtime_error("Cannot open output file: " + path);
    }

    ofs << std::setprecision(10) << std::scientific;
    for (std::size_t i = 0; i < data.size(); ++i) {
        ofs << i << " " << data[i] << "\n";
    }

    if (!ofs.good()) {
        throw std::runtime_error("Failed to write output file: " + path);
    }
}

void dumpTensor(const std::string& prefix,
                const char* name,
                const std::vector<float>& data)
{
    const std::string bin_path = prefix + "_" + name + ".bin";
    const std::string txt_path = prefix + "_" + name + ".txt";

    dumpBinary(bin_path, data);
    dumpText(txt_path, data);

    std::cout << "dumped " << name << ":\n";
    std::cout << "  binary: " << bin_path << "\n";
    std::cout << "  text  : " << txt_path << "\n";
}

void printUsage(const char* program)
{
    std::cerr
        << "Usage:\n  " << program
        << " <model.onnx> <rgb.jpg> <ir.jpg> [dump_prefix]\n\n"
        << "Example:\n  " << program
        << " model.onnx rgb_test.jpg ir_test.jpg onnx_compare\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5) {
        printUsage(argv[0]);
        return 1;
    }

    const std::string model_path = argv[1];
    const std::string rgb_path = argv[2];
    const std::string ir_path = argv[3];
    const std::string dump_prefix = (argc >= 5) ? argv[4] : "onnx_compare";

    try {
        std::cout << "============================================================\n";
        std::cout << "ONNX RGBT RT-DETR RAW OUTPUT TEST\n";
        std::cout << "============================================================\n";
        std::cout << "model : " << model_path << "\n";
        std::cout << "RGB   : " << rgb_path << "\n";
        std::cout << "IR    : " << ir_path << "\n";

        // --------------------------------------------------------
        // 1. Initialize ONNX Runtime
        // --------------------------------------------------------
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "onnx_rgbt_raw_compare");

        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        const auto init_begin = Clock::now();
        Ort::Session session(env, model_path.c_str(), options);
        const auto init_end = Clock::now();

        Ort::AllocatorWithDefaultOptions allocator;

        const std::size_t input_count = session.GetInputCount();
        const std::size_t output_count = session.GetOutputCount();

        if (input_count != 1) {
            throw std::runtime_error("Expected exactly one ONNX input");
        }

        Ort::AllocatedStringPtr input_name_ptr =
            session.GetInputNameAllocated(0, allocator);
        const std::string input_name = input_name_ptr.get();

        const Ort::TypeInfo input_type_info = session.GetInputTypeInfo(0);
        const auto input_tensor_info = input_type_info.GetTensorTypeAndShapeInfo();
        const std::vector<int64_t> input_shape = input_tensor_info.GetShape();

        std::cout << "\n[1] ONNX model\n";
        std::cout << "Input count  : " << input_count << "\n";
        std::cout << "Output count : " << output_count << "\n";
        std::cout << "Init time    : " << std::fixed << std::setprecision(3)
                  << elapsedMs(init_begin, init_end) << " ms\n";
        std::cout << "Input name   : " << input_name << "\n";
        std::cout << "Input shape  : ";
        printShape(input_shape);
        std::cout << "\n";

        if (input_shape.size() != 4 ||
            input_shape[0] != 1 ||
            input_shape[1] != 6) {
            throw std::runtime_error(
                "Unexpected ONNX input shape, expected [1,6,H,W]");
        }

        std::vector<std::string> output_names;
        std::vector<std::vector<int64_t>> output_shapes;
        output_names.reserve(output_count);
        output_shapes.reserve(output_count);

        for (std::size_t i = 0; i < output_count; ++i) {
            Ort::AllocatedStringPtr name_ptr =
                session.GetOutputNameAllocated(i, allocator);
            output_names.emplace_back(name_ptr.get());

            const auto type_info = session.GetOutputTypeInfo(i);
            const auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
            output_shapes.emplace_back(tensor_info.GetShape());

            std::cout << "output[" << i << "] name=" << output_names.back()
                      << " shape=";
            printShape(output_shapes.back());
            std::cout << "\n";
        }

        // --------------------------------------------------------
        // 2. Load images
        // --------------------------------------------------------
        std::cout << "\n[2] Load input images\n";
        const rkcam::ai::Image rgb = rkcam::ai::ImageLoader::loadRgb(rgb_path);
        const rkcam::ai::Image ir = rkcam::ai::ImageLoader::loadRgb(ir_path);

        std::cout << "RGB: " << rgb.width << "x" << rgb.height
                  << " channels=" << rgb.channels << "\n";
        std::cout << "IR : " << ir.width << "x" << ir.height
                  << " channels=" << ir.channels << "\n";

        if (rgb.width != ir.width || rgb.height != ir.height) {
            std::cout << "[WARNING] RGB/IR original sizes differ\n";
        }

        // --------------------------------------------------------
        // 3. Build the SAME preprocessor input metadata used by ONNX.
        // Important: ONNX is [N,C,H,W] = NCHW.
        // --------------------------------------------------------
        std::cout << "\n[3] Preprocess\n";
        TensorAttr model_input;
        model_input.index = 0;
        model_input.name = input_name;
        model_input.shape.dims = input_shape;
        model_input.layout = TensorLayout::NCHW;
        model_input.data_type = TensorDataType::Float32;
        model_input.quantization_type =
            rkcam::ai::TensorQuantizationType::None;
        model_input.element_count = model_input.shape.elementCount();
        model_input.byte_size =
            model_input.element_count * sizeof(float);
        model_input.byte_size_with_stride = model_input.byte_size;
        model_input.zero_point = 0;
        model_input.scale = 1.0f;

        rkcam::ai::RgbtPreprocessor preprocessor;
        const auto prep_begin = Clock::now();
        rkcam::ai::PreprocessResult prep =
            preprocessor.process(rgb, ir, model_input);
        const auto prep_end = Clock::now();

        std::cout << "tensor shape: ";
        printShape(prep.tensor.shape);
        std::cout << "\n";
        std::cout << "tensor values: " << prep.tensor.data.size() << "\n";
        std::cout << "Preprocess time: "
                  << elapsedMs(prep_begin, prep_end) << " ms\n";
        printFirstValues("preprocess tensor (NCHW)", prep.tensor.data);

        if (prep.tensor.shape.dims != input_shape) {
            throw std::runtime_error(
                "Preprocess output shape does not match ONNX input shape");
        }

        // --------------------------------------------------------
        // 4. Create ONNX Runtime input tensor
        // --------------------------------------------------------
        Ort::MemoryInfo memory_info =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
            memory_info,
            prep.tensor.data.data(),
            prep.tensor.data.size(),
            input_shape.data(),
            input_shape.size());

        // --------------------------------------------------------
        // 5. ONNX inference
        // --------------------------------------------------------
        std::cout << "\n[4] ONNX inference\n";

        std::vector<const char*> input_names = {input_name.c_str()};
        std::vector<const char*> output_name_ptrs;
        output_name_ptrs.reserve(output_names.size());
        for (const auto& name : output_names) {
            output_name_ptrs.push_back(name.c_str());
        }

        const auto infer_begin = Clock::now();
        auto ort_outputs = session.Run(
            Ort::RunOptions{nullptr},
            input_names.data(),
            &input_tensor,
            1,
            output_name_ptrs.data(),
            output_name_ptrs.size());
        const auto infer_end = Clock::now();

        std::cout << "Inference time: "
                  << elapsedMs(infer_begin, infer_end) << " ms\n";
        std::cout << "Returned outputs: " << ort_outputs.size() << "\n";

        OutputIndices indices = findOutputs(output_names, output_shapes);

        std::cout << "pred_logits -> output[" << indices.logits << "]\n";
        std::cout << "pred_boxes  -> output[" << indices.boxes << "]\n";

        auto extractFloatOutput =
            [&](std::size_t index) -> std::vector<float> {
                auto& value = ort_outputs[index];
                const auto info = value.GetTensorTypeAndShapeInfo();
                const ONNXTensorElementDataType type = info.GetElementType();
                if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
                    throw std::runtime_error(
                        "Expected ONNX output type FLOAT for raw comparison");
                }
                const std::size_t count = info.GetElementCount();
                const float* ptr = value.GetTensorData<float>();
                if (ptr == nullptr && count != 0) {
                    throw std::runtime_error("ONNX output data pointer is null");
                }
                return std::vector<float>(ptr, ptr + count);
            };

        const std::vector<float> logits = extractFloatOutput(indices.logits);
        const std::vector<float> boxes = extractFloatOutput(indices.boxes);

        std::cout << "\npred_logits shape: ";
        printShape(output_shapes[indices.logits]);
        std::cout << "\n";
        std::cout << "pred_boxes shape : ";
        printShape(output_shapes[indices.boxes]);
        std::cout << "\n";

        // --------------------------------------------------------
        // 6. Print RAW outputs -- compare these with RKNN.
        // --------------------------------------------------------
        printFirstValues("pred_logits", logits, 20);
        printFirstValues("pred_boxes", boxes, 20);
        printStats("pred_logits", logits);
        printStats("pred_boxes", boxes);

        // Full raw outputs: exact float32 bytes + human-readable text.
        dumpTensor(dump_prefix, "pred_logits", logits);
        dumpTensor(dump_prefix, "pred_boxes", boxes);

        std::cout << "\n============================================================\n";
        std::cout << "ONNX RAW OUTPUT TEST FINISHED\n";
        std::cout << "============================================================\n";
        std::cout << "Compare these files with RKNN:\n";
        std::cout << "  " << dump_prefix << "_pred_logits.bin\n";
        std::cout << "  " << dump_prefix << "_pred_boxes.bin\n";

        return 0;
    }
    catch (const Ort::Exception& e) {
        std::cerr << "\n[ONNX RUNTIME ERROR] " << e.what() << "\n";
        return 11;
    }
    catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        return 10;
    }
}