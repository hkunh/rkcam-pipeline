#pragma once
#include <memory>
#include <string>
#include <vector>

#include "rkcam/ai/types.hpp"

namespace rkcam::ai{

class RknnEngine {
public:
    explicit RknnEngine(std::string model_path);
    ~RknnEngine();

    RknnEngine(const RknnEngine&) = delete;
    RknnEngine& operator=(const RknnEngine&) = delete;

    RknnEngine(RknnEngine&&) noexcept;
    RknnEngine& operator=(RknnEngine&&) noexcept;

    bool init();
    void close() noexcept;

    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] const std::string& modelPath() const noexcept;
    [[nodiscard]] const std::string& lastError() const noexcept;

    [[nodiscard]] const std::string& apiVersion() const noexcept;
    [[nodiscard]] const std::string& driverVersion() const noexcept;

    [[nodiscard]] std::size_t inputCount() const noexcept;
    [[nodiscard]] std::size_t outputCount() const noexcept;

    [[nodiscard]] const TensorAttr& inputAttr(std::size_t index = 0) const;
    [[nodiscard]] const TensorAttr& outputAttr(std::size_t index = 0) const;

    [[nodiscard]] const std::vector<TensorAttr>& inputAttrs() const noexcept;
    [[nodiscard]] const std::vector<TensorAttr>& outputAttrs() const noexcept;

    bool infer(
        const FloatTensor& input,
        std::vector<FloatTensor>& outputs
    );

    bool infer(
        const std::vector<FloatTensor>& inputs,
        std::vector<FloatTensor>& outputs
    );

private:
    bool inferImpl(
        const FloatTensor* inputs,
        std::size_t input_count,
        std::vector<FloatTensor>& outputs
    );

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}//namespace rkcam