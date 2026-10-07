// #include "rkcam/ai/image_loader.hpp"
// #include "rkcam/ai/rgbt_preprocessor.hpp"
// #include "rkcam/ai/rknn_engine.hpp"
// #include "rkcam/ai/rtdetr_postprocessor.hpp"

// #include <algorithm>
// #include <chrono>
// #include <cstddef>
// #include <exception>
// #include <iomanip>
// #include <iostream>
// #include <stdexcept>
// #include <string>
// #include <utility>
// #include <vector>

// namespace {

// using rkcam::ai::Detection;
// using rkcam::ai::FloatTensor;
// using rkcam::ai::RknnEngine;
// using rkcam::ai::TensorAttr;
// using rkcam::ai::TensorDataType;
// using rkcam::ai::TensorLayout;
// using rkcam::ai::TensorShape;

// // ============================================================
// // Tensor information helpers
// // ============================================================
// const char* layoutToString(TensorLayout layout) noexcept
// {
//     switch (layout)
//     {
//         case TensorLayout::NCHW:
//             return "NCHW";
//         case TensorLayout::NHWC:
//             return "NHWC";
//         case TensorLayout::NC1HWC2:
//             return "NC1HWC2";
//         case TensorLayout::Unknown:
//         default:
//             return "Unknown";
//     }

// }

// const char* dataTypeToString(
//     TensorDataType type) noexcept
// {
//     switch (type) {

//     case TensorDataType::Float32:
//         return "Float32";

//     case TensorDataType::Float16:
//         return "Float16";

//     case TensorDataType::Int8:
//         return "Int8";

//     case TensorDataType::UInt8:
//         return "UInt8";

//     case TensorDataType::Int32:
//         return "Int32";

//     case TensorDataType::UInt32:
//         return "UInt32";

//     case TensorDataType::Int64:
//         return "Int64";

//     case TensorDataType::UInt64:
//         return "UInt64";

//     case TensorDataType::Unknown:
//     default:
//         return "Unknown";
//     }
// }

// void printShape(const TensorShape& shape)
// {
//     std::cout << "[";
//     for(std::size_t i = 0; i < shape.dims.size(); ++i)
//     {
//         std::cout << shape.dims[i];
//         if(i + 1 < shape.dims.size())
//         {
//             std::cout << ", ";
//         }
//     }
// }

// void printTensorAttr(const char* prefix, const TensorAttr& attr)
// {
//     std::cout << prefix << "[" << attr.index << "]" << "\n";
//     std::cout << " name         : " << attr.name << "\n";
//     std::cout << " shape        : ";
//     printShape(attr.shape);
//     std::cout << "\n";
//     std::cout << " layout       : " << layoutToString(attr.layout) << "\n";
//     std::cout << " data type    :" << dataTypeToString(attr.data_type) << "\n";
//     std::cout << " elements     :" << attr.element_count << "\n";
//     std::cout << " bytes        :" << attr.byte_size << "\n";
//     std::cout << " stride bytes :" << attr.byte_size_with_stride << "\n";
//     std::cout << " zero_point   :" << attr.zero_point << "\n";
//     std::cout << " scale        :" << attr.scale << "\n";        
// }



// // ============================================================
// // Timing helper
// // ============================================================

// using Clock = std::chrono::steady_clock;
// double elapsedMs(Clock::time_point begin, Clock::time_point end)
// {
//     return std::chrono::duration<double, std::milli>(end - begin).count();
// }
// // ============================================================
// // Locate RT-DETR outputs
// //
// // 优先使用 output name：
// //   pred_logits
// //   pred_boxes
// //
// // 如果 RKNN 转换后名字变化，
// // 再根据当前模型 shape fallback：
// //
// //   logits: [1,Q,C]
// //   boxes : [1,Q,4]
// // ============================================================

// struct RtdetrOutputIndices{
//     std::size_t logits = 0;
//     std::size_t boxes = 0;
// };

// bool contains(const std::string& text, const std::string& target)
// {
//     //判断text中是否包含target
//     return text.find(target) != std::string::npos;
// }

// RtdetrOutputIndices findRtdetrOutputs(
//     const RknnEngine& engine,
//     const std::vector<FloatTensor>& outputs
// )
// {
//     if(outputs.size() != engine.outputCount())
//     {
//         throw std::runtime_error(
//             "Output tensor count does not match RKNN model output count"
//         );
//     }
//     constexpr std::size_t kInvalid = static_cast<std::size_t>(-1); //无效状态索引值
//     std::size_t logits_index = kInvalid;
//     std::size_t boxes_index = kInvalid;

//     // --------------------------------------------------------
//     // 1. First try output names
//     // --------------------------------------------------------
//     for(std::size_t i = 0; i < engine.outputCount(); ++i)
//     {
//         const auto& attr = engine.outputAttr(i);
//         if(contains(attr.name, "pred_logits"))
//         {
//             logits_index = i;
//         }
//         if(contains(attr.name, "pred_boxes"))
//         {
//             boxes_index = i;
//         }
//     }
//     if (logits_index != kInvalid &&
//         boxes_index != kInvalid) {

//         return {
//             logits_index,
//             boxes_index
//         };
//     }
//     return {
//         logits_index,
//         boxes_index
//     };
//     // // --------------------------------------------------------
//     // // 2. Fallback by output shape
//     // //
//     // // 当前模型：
//     // //
//     // // logits:
//     // //   [1,300,3]
//     // //
//     // // boxes:
//     // //   [1,300,4]
//     // // --------------------------------------------------------

//     // for (std::size_t i = 0;
//     //      i < outputs.size();
//     //      ++i) {

//     //     const auto& shape =
//     //         outputs[i].shape;

//     //     if (shape.rank() != 3) {
//     //         continue;
//     //     }

//     //     if (shape.dims[2] == 4) {

//     //         if (boxes_index == kInvalid) {
//     //             boxes_index = i;
//     //         }

//     //     } else {

//     //         if (logits_index == kInvalid) {
//     //             logits_index = i;
//     //         }
//     //     }
//     // }


//     // if (logits_index == kInvalid ||
//     //     boxes_index == kInvalid) {

//     //     throw std::runtime_error(
//     //         "Unable to identify RT-DETR "
//     //         "pred_logits / pred_boxes outputs");
//     // }


//     // return {
//     //     logits_index,
//     //     boxes_index
//     // };

// }


// // ============================================================
// // Print detections
// // ============================================================
// void printDetections(
//     const std::vector<Detection>& detections,
//     std::size_t print_topk
// )
// {
//     const std::size_t count = std::min(print_topk, detections.size());
//     std::cout << "\n";
//     std::cout << "Detection     :" << detections.size() << "\n";
//     std::cout << "Showing top   :" << count << "\n";
//     std::cout << "------------------------------------------------------------" << "\n";
//     std::cout << "idx   |" ;
//     std::cout << "class |" ;
//     std::cout << "score |" ;
//     std::cout << "bbox [x1, y1, x2, y2]" << "\n";
//     std::cout << "------------------------------------------------------------" << "\n";

//     for(std::size_t i = 0; i < count; ++i)
//     {
//         const auto& det = detections[i];
//         std::cout 
//             << std::setw(6) << i << "|"
//             << std::setw(6) << det.class_id << "|"
//             << std::setw(6) << det.score << "|"
//             << std::fixed << std::setprecision(6) << det.score << "|"
//             << std::setprecision(2) << "["
//             << det.x1 << ", "
//             << det.y1 << ", "
//             << det.x2 << ", "
//             << det.y2 << ", "
//             << "]" << "\n";
//     }
// }
// // ============================================================
// // Usage
// // ============================================================
// void printUsage(const char* program)
// {
//     std::cerr
//         << "Usage:      \n\n"
//         << " " << program
//         << " <model.rknn>"
//         << " <rgb.jpg>"
//         << " <ir.jpg>"
//         << " [score_threshold]"
//         << " [print_topk]\n\n"
//         << "Example: \n\n"
//         << " " << program
//         << " model.rknn"
//         << " rgb_test.jpg"
//         << " ir_test.jpg"
//         << " 0.0"
//         << " 20\n";
// }
// void printStats(const char* name, const std::vector<float>& data)
// {
//     if (data.empty()) {
//         std::cout << name << ": empty\n";
//         return;
//     }

//     float min_value = std::numeric_limits<float>::infinity();
//     float max_value = -std::numeric_limits<float>::infinity();
//     long double sum = 0.0L;

//     for (const float value : data) {
//         min_value = std::min(min_value, value);
//         max_value = std::max(max_value, value);
//         sum += value;
//     }

//     const double mean = static_cast<double>(sum / data.size());

//     std::cout << name << " stats:\n";
//     std::cout << "  count = " << data.size() << "\n";
//     std::cout << "  min   = " << std::setprecision(10) << min_value << "\n";
//     std::cout << "  max   = " << std::setprecision(10) << max_value << "\n";
//     std::cout << "  mean  = " << std::setprecision(10) << mean << "\n";
// }

// void printFirstValues(const char* name,
//                       const std::vector<float>& data,
//                       std::size_t count = 20)
// {
//     const std::size_t n = std::min(count, data.size());
//     std::cout << "\n" << name << " first " << n << " values:\n";
//     std::cout << std::setprecision(10);
//     for (std::size_t i = 0; i < n; ++i) {
//         std::cout << "  [" << i << "] = " << data[i] << "\n";
//     }
// }
// } //namespace

// int main(int argc, char** argv)
// {
//     // --------------------------------------------------------
//     // Arguments
//     //
//     // mandatory:
//     //   model
//     //   RGB
//     //   IR
//     //
//     // optional:
//     //   score threshold
//     //   number of results to print
//     // --------------------------------------------------------
//     if(argc < 4 || argc > 6)
//     {
//         printUsage(argv[0]);
//     }

//     const std::string model_path = argv[1];
//     const std::string rgb_path = argv[2];
//     const std::string ir_path = argv[3];
    
//     float score_threshold = 0.0f;
//     std::size_t print_topk = 20;
//     try{
//         if(argc >= 5)
//         {
//             score_threshold = std::stof(argv[4]);
//         }
//         if(argc >= 6)
//         {
//             print_topk = static_cast<std::size_t>(std::stoul(argv[5]));
//         }
//     }
//     catch (const std::exception& e)
//     {
//         std::cerr << "Invalid argument: " << e.what() << "\n";
//         return 1;
//     }
//     if (score_threshold < 0.0f ||
//         score_threshold > 1.0f) {

//         std::cerr
//             << "score_threshold must be in [0, 1]\n";

//         return 1;
//     }

//     try{
//         std::cout
//             << "============================================================"
//             << "\n";

//         std::cout
//             << "RKNN RGBT RT-DETR IMAGE TEST"
//             << "\n";

//         std::cout
//             << "============================================================"
//             << "\n";

//         std::cout
//             << "model   :"
//             << model_path
//             << "\n";
//         std::cout
//             << "RGB     :"
//             << rgb_path
//             << "\n";
//         std::cout
//             << "IR      :"
//             << ir_path
//             << "\n";
//         std::cout
//             << "score_threshold:"
//             << score_threshold
//             << "\n";
//         // ====================================================
//         // 1. Initialize RKNN model
//         // ====================================================

//         std::cout << "\n";
//         std::cout
//             << "[1] Initialize RKNN model"
//             << "\n";

//         rkcam::ai::RknnEngine engine(model_path);

//         const auto init_begin = Clock::now();
//         if(!engine.init())
//         {
//             std::cerr
//                 << "RKNN init failed: "
//                 << engine.lastError()
//                 << "\n";
//             return 2;
//         }
//         const auto init_end = Clock::now();
//         std::cout
//             << "RKNN API version    :"
//             << engine.apiVersion() << "\n";
//         std::cout
//             << "RKNN driver version :"
//             << engine.driverVersion() << "\n";
//         std::cout
//             << "Input count         :"
//             << engine.inputCount() << "\n";
//         std::cout
//             << "Output count        :"
//             << engine.outputCount() << "\n";
//         std::cout
//             <<"Model init time      :"
//             << std::fixed << std::setprecision(3)
//             << elapsedMs(init_begin, init_end)
//             << "ms\n";
//         // ====================================================
//         // 2. Print tensor attributes
//         // ====================================================
//         std::cout << "\n";
//         std::cout
//             << "[2] Model tensor attributes"
//             << "\n";
//         for (std::size_t i = 0;
//              i < engine.inputCount();
//              ++i) {

//             printTensorAttr(
//                 "input",
//                 engine.inputAttr(i));
//         }
//         for (std::size_t i = 0;
//              i < engine.outputCount();
//              ++i) {

//             printTensorAttr(
//                 "output",
//                 engine.outputAttr(i));
//         }

//         // ====================================================
//         // 3. Load RGB / IR images
//         // ====================================================
//         std::cout << "\n";
//         std::cout << "[3] Load input images" << "\n";
//         const auto load_begin = Clock::now();
//         const rkcam::ai::Image rgb = rkcam::ai::ImageLoader::loadRgb(rgb_path);
//         const rkcam::ai::Image ir = rkcam::ai::ImageLoader::loadRgb(ir_path);

//         const auto load_end = Clock::now();
//         std::cout
//             << "RGB image: "
//             << rgb.width
//             << "x"
//             << rgb.height
//             << " channels="
//             << rgb.channels
//             << "\n";

//         std::cout
//             << "IR image : "
//             << ir.width
//             << "x"
//             << ir.height
//             << " channels="
//             << ir.channels
//             << "\n";
//         if (rgb.width != ir.width ||
//             rgb.height != ir.height) {
//             std::cout
//                 << "[WARNING] RGB/IR original sizes differ"
//                 << "\n";
//         }
//         std::cout 
//             << "Image load time: "
//             << elapsedMs(load_begin, load_end)
//             << " ms\n";
//         // ====================================================
//         // 4. Preprocess
//         // ====================================================
//         std::cout << "\n";
//         std::cout << "[4] RGBT preprocessing" << "\n";
//         rkcam::ai::RgbtPreprocessor preprocessor;
//         const auto preprocess_begin = Clock::now();
//         const rkcam::ai::PreprocessResult prep = preprocessor.process(rgb, ir, engine.inputAttr(0)); //这里rtdetr把rgb ir整合成[B, 6, H, W];
//         const auto preprocess_end = Clock::now();
//         std::cout
//             << "original size: "
//             << prep.info.original_width
//             << "x"
//             <<  prep.info.original_height
//             << "\n";
//         std::cout
//             << "model size  : "
//             << prep.info.model_width
//             << "x"
//             << prep.info.model_height
//             << "\n";
//         std::cout
//             << "scale_x     : "
//             << prep.info.scale_x
//             << "\n";
//         std::cout
//             << "scale_y     : "
//             << prep.info.scale_y
//             << "\n";
//         std::cout
//             << "pad_x       : "
//             << prep.info.pad_x
//             << "\n";
//         std::cout
//             << "pad_y       : "
//             << prep.info.pad_y
//             << "\n";
//         std::cout << "tensor shape : ";
//         printShape(prep.tensor.shape);
//         std::cout << "\n";
//         std::cout
//             << "tensor values: "
//             << prep.tensor.data.size()
//             << "\n";
//         std::cout
//             << "Preprocess time: "
//             << elapsedMs(
//                    preprocess_begin,
//                    preprocess_end)
//             << " ms\n";
//         // ====================================================
//         // 5. RKNN inference
//         // ====================================================
//         std::cout << "\n" << "[5] RKNN inference" << "\n";
        
//         std::vector<FloatTensor> outputs;
//         const auto inference_begin = Clock::now();
//         if(!engine.infer(prep.tensor, outputs))
//         {
//             std::cerr 
//                 << "RKNN inference failed: "
//                 << engine.lastError()
//                 << "\n";
//             return 3;
//         }
//         const auto inference_end = Clock::now();
//         std::cout
//             << "Inference time: "
//             << elapsedMs(
//                    inference_begin,
//                    inference_end)
//             << " ms\n";
//         std::cout 
//             << "Returned outputs: "
//             << outputs.size()
//             << "\n";
//         for(std::size_t i = 0; i < outputs.size(); ++i)
//         {
//             std::cout << "output tensor [" << i << "] shape=";
//             printShape(outputs[i].shape);
//             std::cout << "data_size=" << outputs[i].data.size() << "\n";            
//         }


//         // ====================================================
//         // 6. Locate RT-DETR outputs
//         // ====================================================
//         const auto output_indices = findRtdetrOutputs(engine, outputs);
//         std::cout
//             << "\nRT-DETR output mapping:"
//             << "\n";
//         std::cout
//             << "  pred_logits -> output["
//             << output_indices.logits
//             << "]\n";
//         std::cout
//             << "  pred_boxes  -> output["
//             << output_indices.boxes
//             << "]\n";
//         // ====================================================
//         // 7. RT-DETR postprocess
//         // ====================================================
//         std::cout << "\n";
//         std::cout << "[6] RT-DETR postprocess" << "\n";
//         rkcam::ai::RtdetrPostprocessConfig post_config;
//         // 0:
//         // 使用模型 query 数作为 Top-K。
//         post_config.num_top_queries = 0;
//         post_config.score_threshold = score_threshold;
//         rkcam::ai::RtdetrPostprocessor postprocessor(post_config);
//         const auto post_begin = Clock::now();
//         const std:: vector<Detection> detections = 
//             postprocessor.process(
//                 outputs[output_indices.logits],
//                 outputs[output_indices.boxes],
//                 prep.info
//             );
//         const auto post_end = Clock::now();
//         std::cout
//             << "Postprocess time: "
//             << elapsedMs(
//                    post_begin,
//                    post_end)
//             << " ms\n";
//         // ====================================================
//         // 8. Print final detections
//         // ====================================================
//         printDetections(detections, print_topk);

//         // ====================================================
//         // Summary
//         // ====================================================
//         std::cout << "\n";
//         std::cout
//             << "============================================================"
//             << "\n";
//         std::cout
//             << "TEST FINISHED"
//             << "\n";
//         std::cout
//             << "============================================================"
//             << "\n";

//         const FloatTensor& logits = outputs[output_indices.logits];
//         const FloatTensor& boxes = outputs[output_indices.boxes];

//         std::cout << "\npred_logits shape: ";
//         printShape(logits.shape);
//         std::cout << "\n";
//         std::cout << "pred_boxes shape : ";
//         printShape(boxes.shape);
//         std::cout << "\n";

//         // --------------------------------------------------------
//         // 5. Print raw output -- THIS is what should be compared
//         // --------------------------------------------------------
//         printFirstValues("pred_logits", logits.data, 20);
//         printFirstValues("pred_boxes", boxes.data, 20);
//         printStats("pred_logits", logits.data);
//         printStats("pred_boxes", boxes.data);


//         std::cout << "\n============================================================\n";
//         std::cout << "RKNN RAW OUTPUT TEST FINISHED\n";
//         std::cout << "============================================================\n";
//         return 0;
//     }
//     catch (const std::exception& e)
//     {
//         std::cerr << "\n[FATAL] " << e.what() << "\n";
//         return 10;
//     }

// }
#include "rkcam/ai/image_loader.hpp"
#include "rkcam/ai/rgbt_preprocessor.hpp"
#include "rkcam/ai/rknn_engine.hpp"

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

using rkcam::ai::FloatTensor;
using rkcam::ai::RknnEngine;
using rkcam::ai::TensorDataType;
using rkcam::ai::TensorLayout;
using rkcam::ai::TensorShape;

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point begin, Clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void printShape(const TensorShape& shape)
{
    std::cout << "[";
    for (std::size_t i = 0; i < shape.dims.size(); ++i) {
        std::cout << shape.dims[i];
        if (i + 1 < shape.dims.size()) {
            std::cout << ", ";
        }
    }
    std::cout << "]";
}

const char* layoutToString(TensorLayout layout) noexcept
{
    switch (layout) {
        case TensorLayout::NCHW: return "NCHW";
        case TensorLayout::NHWC: return "NHWC";
        case TensorLayout::NC1HWC2: return "NC1HWC2";
        case TensorLayout::Unknown:
        default: return "Unknown";
    }
}

const char* dataTypeToString(TensorDataType type) noexcept
{
    switch (type) {
        case TensorDataType::Float32: return "Float32";
        case TensorDataType::Float16: return "Float16";
        case TensorDataType::Int8: return "Int8";
        case TensorDataType::UInt8: return "UInt8";
        case TensorDataType::Int32: return "Int32";
        case TensorDataType::UInt32: return "UInt32";
        case TensorDataType::Int64: return "Int64";
        case TensorDataType::UInt64: return "UInt64";
        case TensorDataType::Unknown:
        default: return "Unknown";
    }
}

struct OutputIndices {
    std::size_t logits = static_cast<std::size_t>(-1);
    std::size_t boxes = static_cast<std::size_t>(-1);
};

bool contains(const std::string& text, const std::string& target)
{
    return text.find(target) != std::string::npos;
}

OutputIndices findOutputs(const RknnEngine& engine,
                          const std::vector<FloatTensor>& outputs)
{
    if (outputs.size() != engine.outputCount()) {
        throw std::runtime_error("RKNN output count mismatch");
    }

    OutputIndices result;

    // 1. Prefer output names.
    for (std::size_t i = 0; i < engine.outputCount(); ++i) {
        const auto& attr = engine.outputAttr(i);
        if (contains(attr.name, "pred_logits")) {
            result.logits = i;
        }
        if (contains(attr.name, "pred_boxes")) {
            result.boxes = i;
        }
    }

    // 2. Fallback to shape [1,Q,C] and [1,Q,4].
    if (result.logits == static_cast<std::size_t>(-1) ||
        result.boxes == static_cast<std::size_t>(-1)) {
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const auto& shape = outputs[i].shape;
            if (shape.rank() != 3 || shape.dims.size() != 3) {
                continue;
            }
            if (shape.dims[2] == 4 &&
                result.boxes == static_cast<std::size_t>(-1)) {
                result.boxes = i;
            } else if (shape.dims[2] == 3 &&
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
                const FloatTensor& tensor)
{
    const std::string bin_path = prefix + "_" + name + ".bin";
    const std::string txt_path = prefix + "_" + name + ".txt";

    dumpBinary(bin_path, tensor.data);
    dumpText(txt_path, tensor.data);

    std::cout << "dumped " << name << ":\n";
    std::cout << "  binary: " << bin_path << "\n";
    std::cout << "  text  : " << txt_path << "\n";
}

void printUsage(const char* program)
{
    std::cerr
        << "Usage:\n  " << program
        << " <model.rknn> <rgb.jpg> <ir.jpg> [dump_prefix]\n\n"
        << "Example:\n  " << program
        << " model.rknn rgb_test.jpg ir_test.jpg rknn_compare\n";
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
    const std::string dump_prefix = (argc >= 5) ? argv[4] : "rknn_compare";

    try {
        std::cout << "============================================================\n";
        std::cout << "RKNN RGBT RT-DETR RAW OUTPUT TEST\n";
        std::cout << "============================================================\n";
        std::cout << "model : " << model_path << "\n";
        std::cout << "RGB   : " << rgb_path << "\n";
        std::cout << "IR    : " << ir_path << "\n";

        // --------------------------------------------------------
        // 1. Initialize RKNN
        // --------------------------------------------------------
        RknnEngine engine(model_path);

        const auto init_begin = Clock::now();
        if (!engine.init()) {
            std::cerr << "RKNN init failed: " << engine.lastError() << "\n";
            return 2;
        }
        const auto init_end = Clock::now();

        std::cout << "\n[1] RKNN model\n";
        std::cout << "RKNN API version    : " << engine.apiVersion() << "\n";
        std::cout << "RKNN driver version : " << engine.driverVersion() << "\n";
        std::cout << "Input count         : " << engine.inputCount() << "\n";
        std::cout << "Output count        : " << engine.outputCount() << "\n";
        std::cout << "Model init time     : " << std::fixed << std::setprecision(3)
                  << elapsedMs(init_begin, init_end) << " ms\n";

        for (std::size_t i = 0; i < engine.inputCount(); ++i) {
            const auto& attr = engine.inputAttr(i);
            std::cout << "input[" << i << "] name=" << attr.name
                      << " shape=";
            printShape(attr.shape);
            std::cout << " layout=" << layoutToString(attr.layout)
                      << " type=" << dataTypeToString(attr.data_type) << "\n";
        }

        for (std::size_t i = 0; i < engine.outputCount(); ++i) {
            const auto& attr = engine.outputAttr(i);
            std::cout << "output[" << i << "] name=" << attr.name
                      << " shape=";
            printShape(attr.shape);
            std::cout << " layout=" << layoutToString(attr.layout)
                      << " type=" << dataTypeToString(attr.data_type) << "\n";
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
        // 3. Preprocess -- RKNN layout must come from model metadata.
        // --------------------------------------------------------
        std::cout << "\n[3] Preprocess\n";
        rkcam::ai::RgbtPreprocessor preprocessor;
        const auto prep_begin = Clock::now();
        const rkcam::ai::PreprocessResult prep =
            preprocessor.process(rgb, ir, engine.inputAttr(0));
        const auto prep_end = Clock::now();

        std::cout << "tensor shape: ";
        printShape(prep.tensor.shape);
        std::cout << "\n";
        std::cout << "tensor values: " << prep.tensor.data.size() << "\n";
        std::cout << "Preprocess time: "
                  << elapsedMs(prep_begin, prep_end) << " ms\n";
        printFirstValues("preprocess tensor", prep.tensor.data);

        // --------------------------------------------------------
        // 4. RKNN inference
        // --------------------------------------------------------
        std::cout << "\n[4] RKNN inference\n";
        std::vector<FloatTensor> outputs;

        const auto infer_begin = Clock::now();
        if (!engine.infer(prep.tensor, outputs)) {
            std::cerr << "RKNN inference failed: " << engine.lastError() << "\n";
            return 3;
        }
        const auto infer_end = Clock::now();

        std::cout << "Inference time: "
                  << elapsedMs(infer_begin, infer_end) << " ms\n";
        std::cout << "Returned outputs: " << outputs.size() << "\n";

        const OutputIndices indices = findOutputs(engine, outputs);

        std::cout << "pred_logits -> output[" << indices.logits << "]\n";
        std::cout << "pred_boxes  -> output[" << indices.boxes << "]\n";

        const FloatTensor& logits = outputs[indices.logits];
        const FloatTensor& boxes = outputs[indices.boxes];

        std::cout << "\npred_logits shape: ";
        printShape(logits.shape);
        std::cout << "\n";
        std::cout << "pred_boxes shape : ";
        printShape(boxes.shape);
        std::cout << "\n";

        // --------------------------------------------------------
        // 5. Print raw output -- THIS is what should be compared
        // --------------------------------------------------------
        printFirstValues("pred_logits", logits.data, 20);
        printFirstValues("pred_boxes", boxes.data, 20);
        printStats("pred_logits", logits.data);
        printStats("pred_boxes", boxes.data);

        // Full raw outputs: exact float32 bytes + human-readable text.
        dumpTensor(dump_prefix, "pred_logits", logits);
        dumpTensor(dump_prefix, "pred_boxes", boxes);

        std::cout << "\n============================================================\n";
        std::cout << "RKNN RAW OUTPUT TEST FINISHED\n";
        std::cout << "============================================================\n";
        std::cout << "Compare these files with ONNX:\n";
        std::cout << "  " << dump_prefix << "_pred_logits.bin\n";
        std::cout << "  " << dump_prefix << "_pred_boxes.bin\n";

        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "\n[FATAL] " << e.what() << "\n";
        return 10;
    }
}