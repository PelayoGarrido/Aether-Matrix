#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>
#include <vector>

namespace aether
{
struct PerformanceNote
{
    int pitch=60, velocity=80;
    int64_t tick=0, duration=480, onsetOffsetUs=0, releaseOffsetUs=0;
};
struct PerformancePedal
{
    int64_t tick=0, offsetUs=0;
    int value=0;
};
struct PerformanceEvent
{
    enum Type { off, pedal, on } type=on;
    int64_t micros=0;
    size_t note=0;
    int value=0;
};
class Performance
{
public:
    static Performance load(const juce::File&);
    static Performance fromJson(const juce::var&);
    int bpm=120, bars=8, ppq=480, sharps=0, minor=0;
    std::vector<PerformanceNote> notes;
    std::vector<PerformancePedal> pedals;
    int64_t microsAt(int64_t tick,int64_t offset=0) const;
    std::vector<PerformanceEvent> events() const;
    juce::MidiFile midi() const;
    juce::var canonical() const;
    void validate() const;
};
}
