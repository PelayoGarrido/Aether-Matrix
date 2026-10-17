#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include <vector>

namespace aether
{
// Fixed trained model, offline helper only. No GUI/audio callback operations.
class PianistEngine
{
public:
    static constexpr size_t vocabulary=5389;
    explicit PianistEngine(const juce::File& verifiedAssetDirectory);
    ~PianistEngine();
    void encode(const std::vector<int64_t>& tokens);
    std::array<float,vocabulary> decode(int64_t previousToken);
    std::vector<float> encoderState() const;
    void cancel() noexcept;
    size_t position() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
