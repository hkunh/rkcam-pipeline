#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <onnxruntime_cxx_api.h>

#include "rkcam/ai/rknn_engine.hpp"
#include "rkcam/ai/types.hpp"

namespace fs = std::filesystem;

using rkcam::ai::FloatTensor;
using rkcam::ai::RknnEngine;
using rkcam::ai::TensorLayout;
using rkcam::ai::TensorShape;

namespace {

struct TensorData {
    std::string name;
    std::vector<int64_t> shape;
    TensorLayout layout = TensorLayout::Unknown;
    std::vector<float> data;
};

struct Stats {
    std::size_t count = 0;
    std::size_t finite_count = 0;
    std::size_t nan_count = 0;
    std::size_t inf_count = 0;
    float min_value = std::numeric_limits<float>::infinity();
    float max_value = -std::numeric_limits<float>::infinity();
    double mean = 0.0;
    double l2 = 0.0;
};

const char* layoutName(TensorLayout layout)
{
    switch (layout) {
        case TensorLayout::NCHW: return "NCHW";
        case TensorLayout::NHWC: return "NHWC";
        case TensorLayout::NC1HWC2: return "NC1HWC2";
        default: return "Unknown";
    }
}

void printShape(const std::vector<int64_t>& shape)
{
    std::cout << "[";
    for (std::size_t i = 0; i < shape.size(); ++i) {
        if (i != 0) std::cout << ",";
        std::cout << shape[i];
    }
    std::cout << "]";
}

void printShape(const TensorShape& shape)
{
    printShape(shape.dims);
}

std::vector<float> readFloatFile(const fs::path& path, std::size_t expected_count)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("Failed to open input file: " + path.string());
    }

    const std::streamsize bytes = input.tellg();
    input.seekg(0, std::ios::beg);

    const std::streamsize expected_bytes =
        static_cast<std::streamsize>(expected_count * sizeof(float));

    if (bytes != expected_bytes) {
        std::ostringstream oss;
        oss << "File size mismatch: " << path
            << ", got " << bytes
            << " bytes, expected " << expected_bytes;
        throw std::runtime_error(oss.str());
    }

    std::vector<float> data(expected_count);
    if (!input.read(reinterpret_cast<char*>(data.data()), expected_bytes)) {
        throw std::runtime_error("Failed to read input file: " + path.string());
    }
    return data;
}

void writeFloatFile(const fs::path& path, const std::vector<float>& data)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Failed to create output file: " + path.string());
    }
    if (!data.empty()) {
        output.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size() * sizeof(float)));
    }
    if (!output.good()) {
        throw std::runtime_error("Failed to write output file: " + path.string());
    }
}

FloatTensor makeTensor(std::vector<int64_t> shape,
                       TensorLayout layout,
                       std::vector<float> data)
{
    FloatTensor tensor;
    tensor.shape.dims = std::move(shape);
    tensor.layout = layout;
    tensor.data = std::move(data);
    if (!tensor.valid()) {
        throw std::runtime_error("Constructed FloatTensor is invalid");
    }
    return tensor;
}

Stats computeStats(const std::vector<float>& data)
{
    Stats s;
    s.count = data.size();
    double sum = 0.0;
    double sq = 0.0;

    for (float v : data) {
        if (std::isnan(v)) {
            ++s.nan_count;
            continue;
        }
        if (std::isinf(v)) {
            ++s.inf_count;
            continue;
        }
        ++s.finite_count;
        s.min_value = std::min(s.min_value, v);
        s.max_value = std::max(s.max_value, v);
        const double d = static_cast<double>(v);
        sum += d;
        sq += d * d;
    }

    if (s.finite_count != 0) {
        s.mean = sum / static_cast<double>(s.finite_count);
        s.l2 = std::sqrt(sq);
    }
    return s;
}

void printStats(const TensorData& tensor)
{
    const Stats s = computeStats(tensor.data);
    std::cout << tensor.name << ": shape=";
    printShape(tensor.shape);
    std::cout << " layout=" << layoutName(tensor.layout)
              << " count=" << s.count
              << " finite=" << s.finite_count
              << " nan=" << s.nan_count
              << " inf=" << s.inf_count;
    if (s.finite_count != 0) {
        std::cout << " min=" << s.min_value
                  << " max=" << s.max_value
                  << " mean=" << s.mean
                  << " l2=" << s.l2;
    }
    std::cout << "\n";
}

std::vector<TensorData> runOnnx(const std::string& model_path,
                                const std::array<FloatTensor, 4>& inputs)
{
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "hit_compare");
    Ort::SessionOptions options;
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session(env, model_path.c_str(), options);

    if (session.GetInputCount() != 4) {
        throw std::runtime_error("Expected 4 ONNX inputs");
    }

    Ort::AllocatorWithDefaultOptions allocator;
    const std::array<std::string, 4> expected_input_names{
        "template", "search", "dynamic_template", "template_geometry"
    };
    std::array<std::string, 4> input_names;

    for (std::size_t i = 0; i < 4; ++i) {
        auto name = session.GetInputNameAllocated(i, allocator);
        input_names[i] = name.get();
        if (input_names[i] != expected_input_names[i]) {
            throw std::runtime_error("Unexpected ONNX input name: " + input_names[i]);
        }
    }

    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> ort_inputs;
    ort_inputs.reserve(4);
    for (std::size_t i = 0; i < 4; ++i) {
        ort_inputs.emplace_back(
            Ort::Value::CreateTensor<float>(
                memory_info,
                const_cast<float*>(inputs[i].data.data()),
                inputs[i].data.size(),
                inputs[i].shape.dims.data(),
                inputs[i].shape.dims.size()));
    }

    std::array<const char*, 4> input_name_ptrs{
        input_names[0].c_str(), input_names[1].c_str(),
        input_names[2].c_str(), input_names[3].c_str()
    };

    const std::size_t output_count = session.GetOutputCount();
    if (output_count == 0) {
        throw std::runtime_error("ONNX model has no outputs");
    }

    std::vector<std::string> output_names;
    output_names.reserve(output_count);
    for (std::size_t i = 0; i < output_count; ++i) {
        auto name = session.GetOutputNameAllocated(i, allocator);
        output_names.emplace_back(name.get());
    }

    std::vector<const char*> output_name_ptrs;
    output_name_ptrs.reserve(output_count);
    for (const auto& name : output_names) {
        output_name_ptrs.push_back(name.c_str());
    }

    auto outputs = session.Run(
        Ort::RunOptions{nullptr},
        input_name_ptrs.data(),
        ort_inputs.data(),
        ort_inputs.size(),
        output_name_ptrs.data(),
        output_name_ptrs.size());

    if (outputs.size() != output_count) {
        throw std::runtime_error("ONNX output count mismatch");
    }

    std::vector<TensorData> result;
    result.reserve(output_count);

    for (std::size_t i = 0; i < output_count; ++i) {
        const auto info = outputs[i].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("ONNX output is not float32: " + output_names[i]);
        }

        TensorData tensor;
        tensor.name = output_names[i];
        tensor.shape = info.GetShape();
        tensor.layout = TensorLayout::Unknown;

        const std::size_t count = info.GetElementCount();
        const float* data = outputs[i].GetTensorData<float>();
        tensor.data.assign(data, data + count);
        result.push_back(std::move(tensor));
    }

    return result;
}

std::vector<TensorData> runRknn(const std::string& model_path,
                                const std::array<FloatTensor, 4>& inputs)
{
    RknnEngine engine(model_path, false);
    if (!engine.init()) {
        throw std::runtime_error("RKNN init failed: " + engine.lastError());
    }

    std::cout
        << "\nRKNN runtime information:\n"
        << "  API version    : "
        << engine.apiVersion()
        << "\n"
        << "  Driver version : "
        << engine.driverVersion()
        << "\n";

    std::cout << "\nRKNN model metadata:\n";
    for (std::size_t i = 0; i < engine.inputCount(); ++i) {
        const auto& attr = engine.inputAttr(i);
        std::cout << "  input[" << i << "] " << attr.name << " shape=";
        printShape(attr.shape);
        std::cout << " layout=" << layoutName(attr.layout) << "\n";
    }

    for (std::size_t i = 0; i < engine.outputCount(); ++i) {
        const auto& attr = engine.outputAttr(i);
        std::cout << "  output[" << i << "] " << attr.name << " shape=";
        printShape(attr.shape);
        std::cout << " layout=" << layoutName(attr.layout) << "\n";
    }

    std::vector<FloatTensor> input_vector{
        inputs[0], inputs[1], inputs[2], inputs[3]
    };
    std::vector<FloatTensor> raw_outputs;

    if (!engine.infer(input_vector, raw_outputs)) {
        throw std::runtime_error("RKNN infer failed: " + engine.lastError());
    }

    if (raw_outputs.size() != engine.outputCount()) {
        throw std::runtime_error("RKNN output count mismatch");
    }

    std::vector<TensorData> result;
    result.reserve(raw_outputs.size());

    for (std::size_t i = 0; i < raw_outputs.size(); ++i) {
        TensorData tensor;
        tensor.name = engine.outputAttr(i).name;
        tensor.shape = raw_outputs[i].shape.dims;
        tensor.layout = raw_outputs[i].layout;
        tensor.data = raw_outputs[i].data;
        result.push_back(std::move(tensor));
    }

    return result;
}

TensorData convertNhwcToNchw(const TensorData& src)
{
    if (src.layout != TensorLayout::NHWC || src.shape.size() != 4) {
        throw std::runtime_error("convertNhwcToNchw expects rank-4 NHWC");
    }

    const int64_t n = src.shape[0];
    const int64_t h = src.shape[1];
    const int64_t w = src.shape[2];
    const int64_t c = src.shape[3];

    TensorData dst;
    dst.name = src.name;
    dst.shape = {n, c, h, w};
    dst.layout = TensorLayout::NCHW;
    dst.data.resize(src.data.size());

    for (int64_t ni = 0; ni < n; ++ni)
        for (int64_t ci = 0; ci < c; ++ci)
            for (int64_t yi = 0; yi < h; ++yi)
                for (int64_t xi = 0; xi < w; ++xi) {
                    const std::size_t src_index =
                        static_cast<std::size_t>(((ni * h + yi) * w + xi) * c + ci);
                    const std::size_t dst_index =
                        static_cast<std::size_t>(((ni * c + ci) * h + yi) * w + xi);
                    dst.data[dst_index] = src.data[src_index];
                }

    return dst;
}

TensorData alignRknnToOnnx(const TensorData& onnx, const TensorData& rknn)
{
    // Generic rule: if the shapes already match, compare raw logical order directly.
    // This safely handles rank-1/2/3 tensors whose RKNN layout is Unknown.
    if (onnx.shape == rknn.shape) {
        return rknn;
    }

    // Rank-4 feature/image tensors may need NHWC -> NCHW canonicalization.
    if (onnx.shape.size() == 4 && rknn.shape.size() == 4 &&
        rknn.layout == TensorLayout::NHWC)
    {
        TensorData converted = convertNhwcToNchw(rknn);
        if (onnx.shape == converted.shape) {
            return converted;
        }
    }

    std::ostringstream oss;
    oss << "Cannot align output '" << onnx.name << "': ONNX=";
    for (std::size_t i = 0; i < onnx.shape.size(); ++i) {
        if (i) oss << "x";
        oss << onnx.shape[i];
    }
    oss << ", RKNN=";
    for (std::size_t i = 0; i < rknn.shape.size(); ++i) {
        if (i) oss << "x";
        oss << rknn.shape[i];
    }
    oss << ", RKNN layout=" << layoutName(rknn.layout);
    throw std::runtime_error(oss.str());
}

void compareOne(const TensorData& onnx,
                const TensorData& rknn,
                const fs::path& dump_dir)
{
    TensorData aligned = alignRknnToOnnx(onnx, rknn);
    if (onnx.data.size() != aligned.data.size()) {
        throw std::runtime_error("Element count mismatch for output: " + onnx.name);
    }

    const std::size_t count = onnx.data.size();
    std::vector<float> abs_diff(
        count, std::numeric_limits<float>::quiet_NaN());

    std::size_t finite_pairs = 0;
    std::size_t nonfinite_pairs = 0;
    double sum_abs = 0.0;
    double sum_sq = 0.0;
    double dot = 0.0;
    double onnx_sq = 0.0;
    double rknn_sq = 0.0;
    float max_abs = -1.0F;
    std::size_t max_index = 0;

    for (std::size_t i = 0; i < count; ++i) {
        const float a = onnx.data[i];
        const float b = aligned.data[i];

        if (!std::isfinite(a) || !std::isfinite(b)) {
            ++nonfinite_pairs;
            continue;
        }

        ++finite_pairs;
        const float d = std::fabs(a - b);
        abs_diff[i] = d;
        sum_abs += d;

        const double da = static_cast<double>(a);
        const double db = static_cast<double>(b);
        const double delta = da - db;
        sum_sq += delta * delta;
        dot += da * db;
        onnx_sq += da * da;
        rknn_sq += db * db;

        if (d > max_abs) {
            max_abs = d;
            max_index = i;
        }
    }

    std::cout << "\n============================================================\n"
              << "OUTPUT COMPARISON: " << onnx.name << "\n"
              << "============================================================\n"
              << "shape              = ";
    printShape(onnx.shape);
    std::cout << "\n"
              << "elements           = " << count << "\n"
              << "finite_pairs       = " << finite_pairs << "\n"
              << "nonfinite_pairs    = " << nonfinite_pairs << "\n";

    if (finite_pairs != 0) {
        const double mean_abs = sum_abs / static_cast<double>(finite_pairs);
        const double rmse = std::sqrt(sum_sq / static_cast<double>(finite_pairs));
        const double cosine = dot / (std::sqrt(onnx_sq) * std::sqrt(rknn_sq) + 1e-12);
        const double relative_l2 = std::sqrt(sum_sq) / (std::sqrt(onnx_sq) + 1e-12);

        std::cout << "max_abs_diff       = " << max_abs << "\n"
                  << "mean_abs_diff      = " << mean_abs << "\n"
                  << "rmse               = " << rmse << "\n"
                  << "relative_l2        = " << relative_l2 << "\n"
                  << "cosine             = " << cosine << "\n"
                  << "worst_index        = " << max_index << "\n"
                  << "onnx@worst         = " << onnx.data[max_index] << "\n"
                  << "rknn@worst         = " << aligned.data[max_index] << "\n";
    }

    fs::create_directories(dump_dir);
    writeFloatFile(dump_dir / ("onnx_" + onnx.name + ".bin"), onnx.data);
    writeFloatFile(dump_dir / ("rknn_" + onnx.name + ".bin"), aligned.data);
    writeFloatFile(dump_dir / ("abs_diff_" + onnx.name + ".bin"), abs_diff);
}

void printUsage(const char* program)
{
    std::cout << "Usage:\n"
              << "  " << program
              << " <model.onnx> <model.rknn> [input_prefix] [dump_dir]\n\n"
              << "Defaults:\n"
              << "  input_prefix = ./onnx_hit\n"
              << "  dump_dir     = ./hit_compare_dump\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc < 3 || argc > 5) {
            printUsage(argv[0]);
            return 1;
        }

        const std::string onnx_model = argv[1];
        const std::string rknn_model = argv[2];
        const std::string prefix = (argc >= 4) ? argv[3] : "./onnx_hit";
        const fs::path dump_dir = (argc >= 5) ? fs::path(argv[4])
                                                : fs::path("./hit_compare_dump");

        std::cout << std::fixed << std::setprecision(9);

        std::array<FloatTensor, 4> inputs;
        inputs[0] = makeTensor({1, 6, 128, 128}, TensorLayout::NCHW,
                               readFloatFile(prefix + "_template.bin", 1U * 6U * 128U * 128U));
        inputs[1] = makeTensor({1, 6, 256, 256}, TensorLayout::NCHW,
                               readFloatFile(prefix + "_search.bin", 1U * 6U * 256U * 256U));
        inputs[2] = makeTensor({1, 6, 128, 128}, TensorLayout::NCHW,
                               readFloatFile(prefix + "_dynamic_template.bin", 1U * 6U * 128U * 128U));
        inputs[3] = makeTensor({1, 4}, TensorLayout::Unknown,
                               readFloatFile(prefix + "_template_geometry.bin", 4U));

        std::cout << "============================================================\n"
                  << "Generic HiT ONNX vs RKNN compare\n"
                  << "============================================================\n"
                  << "ONNX        : " << onnx_model << "\n"
                  << "RKNN        : " << rknn_model << "\n"
                  << "input prefix: " << prefix << "\n"
                  << "dump dir    : " << dump_dir << "\n"
                  << "============================================================\n";

        std::cout << "\n[1] ONNX inference\n";
        const auto onnx_outputs = runOnnx(onnx_model, inputs);
        std::cout << "ONNX outputs: " << onnx_outputs.size() << "\n";
        for (const auto& t : onnx_outputs) {
            std::cout << "  ";
            printStats(t);
        }

        std::cout << "\n[2] RKNN inference\n";
        const auto rknn_outputs = runRknn(rknn_model, inputs);
        std::cout << "RKNN outputs: " << rknn_outputs.size() << "\n";
        for (const auto& t : rknn_outputs) {
            std::cout << "  ";
            printStats(t);
        }

        if (onnx_outputs.size() != rknn_outputs.size()) {
            throw std::runtime_error("ONNX/RKNN output counts differ");
        }

        std::map<std::string, std::size_t> rknn_name_to_index;
        for (std::size_t i = 0; i < rknn_outputs.size(); ++i) {
            rknn_name_to_index[rknn_outputs[i].name] = i;
        }

        std::cout << "\n[3] Compare outputs\n";
        for (std::size_t i = 0; i < onnx_outputs.size(); ++i) {
            const auto& onnx = onnx_outputs[i];
            auto it = rknn_name_to_index.find(onnx.name);
            if (it != rknn_name_to_index.end()) {
                compareOne(onnx, rknn_outputs[it->second], dump_dir);
            } else {
                std::cout << "[WARN] Output name '" << onnx.name
                          << "' not found in RKNN; compare by index " << i << "\n";
                compareOne(onnx, rknn_outputs[i], dump_dir);
            }
        }

        std::cout << "\n============================================================\n"
                  << "COMPARE COMPLETED\n"
                  << "============================================================\n"
                  << "Dump directory: " << dump_dir << "\n";
        return 0;
    }
    catch (const Ort::Exception& error) {
        std::cerr << "\n[ORT ERROR] " << error.what() << "\n";
        return 11;
    }
    catch (const std::exception& error) {
        std::cerr << "\n[FAILED] " << error.what() << "\n";
        return 1;
    }
}
