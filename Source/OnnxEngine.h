#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <memory>

class OnnxEngine
{
public:
    OnnxEngine();
    ~OnnxEngine();
    juce::String load(const juce::File&);
    bool render(const std::array<float, 16>&, std::array<float, 2048>&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
