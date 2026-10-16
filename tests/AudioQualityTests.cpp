#include "PluginProcessor.h"
#include "AudioQuality.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <stdexcept>

namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct PlayHead final : juce::AudioPlayHead
{
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position; position.setIsPlaying(playing); position.setBpm(120.0); position.setPpqPosition(0.0);
        return position;
    }
};
double aliasEnergy(bool corrected, bool pulse)
{
    constexpr int size = 4096, fundamental = 997;
    std::vector<float> data(size * 2, 0.0f);
    for (int i = 0; i < size; ++i)
    {
        const double phase = static_cast<double>((i * fundamental) % size) / size;
        const double increment = static_cast<double>(fundamental) / size;
        data[static_cast<size_t>(i)] = pulse
            ? (corrected ? aether::audio::pulse(phase, increment, 0.5) : (phase < 0.5 ? 0.5f : -0.5f))
            : (corrected ? aether::audio::saw(phase, increment) : static_cast<float>(2 * phase - 1));
    }
    juce::dsp::FFT fft(12); fft.performFrequencyOnlyForwardTransform(data.data());
    double alias = 0.0;
    for (int bin = 1; bin < size / 2; ++bin)
        if (bin != fundamental && bin != fundamental * 2)
            alias += static_cast<double>(data[static_cast<size_t>(bin)]) * data[static_cast<size_t>(bin)];
    return alias;
}
void benchmark()
{
    AetherMatrixProcessor processor;
    PlayHead playhead; playhead.playing = true; processor.setPlayHead(&playhead);
    constexpr double rate = 44100.0;
    constexpr int blockSize = 64, blocks = 12000;
    processor.prepareToPlay(rate, blockSize);
    juce::AudioBuffer<float> buffer(2, blockSize); juce::MidiBuffer midi;
    for (int i = 0; i < 200; ++i) processor.processBlock(buffer, midi);
    std::vector<double> samples; samples.reserve(blocks);
    for (int i = 0; i < blocks; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        processor.processBlock(buffer, midi);
        samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
    }
    std::sort(samples.begin(), samples.end());
    const double deadline = blockSize / rate * 1.0e6;
    const auto misses = std::count_if(samples.begin(), samples.end(), [deadline](double x) { return x >= deadline; });
    std::cout << "{\"sample_rate\":44100,\"block_size\":64,\"callbacks\":" << blocks
              << ",\"p50_us\":" << samples[blocks / 2] << ",\"p99_us\":" << samples[blocks * 99 / 100]
              << ",\"max_us\":" << samples.back() << ",\"deadline_us\":" << deadline
              << ",\"deadline_misses\":" << misses << "}\n";
    processor.releaseResources();
}
}
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        if (argc == 2 && juce::String(argv[1]) == "--benchmark") { benchmark(); return 0; }
        for (const bool pulse : {false, true})
        {
            const double naive = aliasEnergy(false, pulse), corrected = aliasEnergy(true, pulse);
            require(corrected < naive * 0.2, "Band-limited oscillator did not reduce alias energy by 7 dB");
            std::cout << (pulse ? "pulse" : "saw") << " alias reduction dB: " << 10 * std::log10(naive / corrected) << '\n';
        }
        const auto reference = aether::audio::Decay::forRate(48000.0, 0.55f);
        for (const double rate : {22050.0, 44100.0, 48000.0, 96000.0, 192000.0})
        {
            const auto d = aether::audio::Decay::forRate(rate, 0.55f);
            const std::array<float, 4> actual {d.lead, d.sustained, d.bass, d.chord};
            const std::array<float, 4> target {reference.lead, reference.sustained, reference.bass, reference.chord};
            for (size_t i = 0; i < actual.size(); ++i)
                require(std::abs(std::pow(actual[i], rate * 0.1) - std::pow(target[i], 4800.0)) < 0.002,
                        "Envelope duration differs across sample rates");
        }
        require(aether::audio::partialGain(25000, 48000) == 0.0f, "Above-Nyquist partial not removed");
        AetherMatrixProcessor processor; PlayHead playhead; processor.setPlayHead(&playhead);
        processor.prepareToPlay(44100, 256);
        processor.apvts.getRawParameterValue("space")->store(0.0f);
        juce::AudioBuffer<float> buffer(2, 256); juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 0);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, 0.8f), 32);
        midi.addEvent(juce::MidiMessage::noteOff(1, 60), 96);
        processor.processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 100, 156) > 0.001f, "Old note-off silenced the newly held note");
        midi.addEvent(juce::MidiMessage::noteOff(2, 64), 0); processor.processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 128, 128) > 0.001f, "Other MIDI channel silenced the held voice");
        midi.addEvent(juce::MidiMessage::noteOff(1, 64), 0); processor.processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 0, 64) > 0.001f, "Note-off hard-muted the voice instead of releasing");
        for (int i = 0; i < 8; ++i) processor.processBlock(buffer, midi);
        require(buffer.getMagnitude(0, 128, 128) < 0.001f, "Held note-off did not finish releasing the voice");
        processor.releaseResources();
        std::cout << "PASS: oscillator spectrum, sample-rate decay, partial suppression, legato note-off\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
