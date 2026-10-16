#pragma once
#include "Performance.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <memory>

namespace aether
{
// Offline instrument renderer. File loading/decoding is deliberately excluded
// from processBlock; the app will supervise this in a separate helper process.
class PianoSampler
{
public:
    PianoSampler(const juce::File& bank,const juce::File& manifest,const juce::File& layout,double sampleRate);
    ~PianoSampler();
    void noteOn(size_t id,const PerformanceNote&);
    void noteOff(size_t id);
    void pedal(int value);
    void render(juce::AudioBuffer<float>&,int frames);
    size_t cacheBytes() const;
    size_t peakVoices() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
