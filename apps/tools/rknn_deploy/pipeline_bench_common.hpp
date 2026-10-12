#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "rkcam/ai/types.hpp"

namespace rkcam::bench {

using Json = nlohmann::json;
namespace fs = std::filesystem;
using rkcam::ai::FloatTensor;
using rkcam::ai::TensorAttr;

void require(bool condition, const std::string& message);
Json readJson(const fs::path& path);
void writeJson(const fs::path& path, const Json& value);
fs::path artifactPath(const fs::path& root, const std::string& relative);
std::string sha256File(const fs::path& path);
void verifyHash(const fs::path& path, const std::string& expected);
std::string safeName(const std::string& name);
std::string utcTimestamp();
std::string uniqueTimestamp();

struct TensorSpec {
    std::string name;
    std::vector<int64_t> shape;
    std::string dtype;
    std::string layout;
    static TensorSpec fromJson(const Json& value);
};

FloatTensor readFloatNpy(const fs::path& path);
void writeFloatNpy(const fs::path& path, const FloatTensor& tensor);
FloatTensor loadTensor(const fs::path& root, const Json& record, const TensorSpec& spec);
void validateTensor(const FloatTensor& tensor, const TensorSpec& spec, bool finite = true);
FloatTensor canonicalOutput(const FloatTensor& tensor, const TensorSpec& spec);
FloatTensor prepareInput(const FloatTensor& tensor, const TensorAttr& attr);
void validateRuntimeAttr(const TensorAttr& attr, const TensorSpec& spec);

struct StagePlan {
    std::string name;
    fs::path modelPath;
    std::string modelHash;
    std::vector<TensorSpec> inputs;
    std::vector<TensorSpec> outputs;
    std::map<std::string, std::string> bindings;
};

struct Bundle {
    fs::path root;
    Json manifest;
    Json build;
    Json sample;
    std::string caseName;
    std::vector<StagePlan> chain;
    std::map<std::string, FloatTensor> external;
    std::vector<std::vector<FloatTensor>> goldenOnnx;
    std::vector<std::vector<FloatTensor>> goldenTorch;
    size_t predictionStage = 0;
    size_t predictionOutput = 0;
};

Bundle loadBundle(const fs::path& root, const std::string& buildKey, const std::string& caseName);
Json compareTensor(const FloatTensor& actual, const FloatTensor& golden, bool boxes,
                   double cosMin, double relMax, double boxAtol);
Json timingStatistics(const std::vector<double>& values);

} // namespace rkcam::bench
