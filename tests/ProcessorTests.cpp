#include "PluginProcessor.h"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <limits>
#include <thread>

namespace
{
thread_local bool countAllocations = false;
thread_local unsigned allocations = 0;
int checks = 0;
void require(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
struct PlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double bpm = 120.0;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setBpm(bpm);
        info.setPpqPosition(0.0);
        return info;
    }
};
}

void* operator new(std::size_t size)
{
    if (countAllocations) ++allocations;
    if (auto* result = std::malloc(std::max(size, std::size_t { 1 }))) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete[](void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { std::free(ptr); }

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getChildFile("aether-tests-" + juce::Uuid().toString());
    try
    {
        {
            AetherMatrixProcessor processor(root);
            PlayHead playhead;
            processor.setPlayHead(&playhead);
            processor.prepareToPlay(48000.0, 512);
            juce::AudioBuffer<float> buffer(2, 512);
            juce::MidiBuffer midi;
            midi.ensureSize(16384);
            processor.processBlock(buffer, midi);
            require(buffer.getMagnitude(0, 512) < 0.0001f, "Stopped transport must start silent");
            const std::array<juce::uint8, 4096> sysex {};
            midi.addEvent(juce::MidiMessage::createSysExMessage(sysex.data(), static_cast<int>(sysex.size())), 0);
            midi.addEvent(juce::MidiMessage::noteOn(1, 60, 0.8f), 120);
            allocations = 0;
            countAllocations = true;
            processor.processBlock(buffer, midi);
            countAllocations = false;
            require(allocations == 0, "Audio callback allocated C++ heap memory");
            require(midi.isEmpty(), "Realtime MIDI output must stay disabled");
            require(buffer.getMagnitude(0, 120) < 0.0001f, "MIDI note must respect its sample offset");
            require(buffer.getMagnitude(0, 120, 392) > 0.001f, "MIDI note must produce audio");
            for (const auto rate : { 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
            {
                processor.prepareToPlay(rate, 512);
                playhead.playing = true;
                playhead.bpm = std::numeric_limits<double>::quiet_NaN();
                for (int block = 0; block < 16; ++block)
                {
                    processor.processBlock(buffer, midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                            require(std::isfinite(buffer.getSample(ch, sample)), "Non-finite audio");
                }
            }
            processor.apvts.getRawParameterValue("tension")->store(std::numeric_limits<float>::quiet_NaN());
            processor.processBlock(buffer, midi);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                require(std::isfinite(buffer.getSample(0, i)), "Non-finite parameter must not poison audio");
            processor.apvts.getRawParameterValue("tension")->store(0.3f);
            require(!processor.loadOnnxModel(root.getChildFile("missing.onnx")), "Missing models must fail");
            auto state = juce::MemoryBlock{};
            processor.getStateInformation(state);
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            processor.setStateInformation(nullptr, 100);
            processor.setStateInformation("invalid", 7);
            const auto weights = processor.getStyleWeights();
            float sum = 0.0f;
            for (float weight : weights) sum += weight;
            require(std::abs(sum - 1.0f) < 0.00001f, "Style weights must sum to one");
            processor.prepareToPlay(48000.0, 512);
            processor.currentHostBpm.store(120.0f);
            processor.triggerMasterGeneration();
            processor.triggerMasterGeneration();
            require(processor.isGenerating(), "Render must be marked busy before dispatch");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (processor.isGenerating() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!processor.isGenerating() && processor.hasGeneratedFiles(), "Offline render failed");
            require(processor.getGenerationProgress() == 1.0f, "Export progress must finish at one");
            const auto wav = processor.getGeneratedWavFile();
            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(wav.createInputStream().release(), true));
            require(reader != nullptr, "WAV reader rejected export");
            require(reader->bitsPerSample == 24 && reader->numChannels == 2, "Export must be stereo PCM24");
            require(reader->sampleRate == 48000.0 && reader->lengthInSamples == 384000, "Export duration must be four bars");
            juce::AudioBuffer<float> exported(2, 384000);
            require(reader->read(&exported, 0, 384000, 0, true, true), "Cannot read export samples");
            require(exported.getMagnitude(0, 384000) > 0.01f, "WAV export is silent");
            reader.reset();
            juce::MidiFile file;
            auto stream = processor.getGeneratedMidiFile().createInputStream();
            require(stream && file.readFrom(*stream), "MIDI reader rejected export");
            require(file.getNumTracks() == 4 && file.getTimeFormat() == 960, "MIDI tracks or TPQN incorrect");
            stream.reset();
            juce::MemoryBlock midiBytes;
            require(processor.getGeneratedMidiFile().loadFileAsData(midiBytes), "Cannot read MIDI bytes");
            const auto* data = static_cast<const unsigned char*>(midiBytes.getData());
            require(midiBytes.getSize() > 14 && data[8] == 0 && data[9] == 1, "MIDI must use SMF Type 1");
            processor.releaseResources();
        }
        {
            OnnxEngine engine;
            const auto error = engine.load(juce::File(AETHER_TEST_MODEL));
#if AETHER_ENABLE_ONNX && (!JUCE_MAC || JUCE_ARM)
            require(error.isEmpty(), "Bundled ONNX decoder must load");
            std::array<float, 16> z {}; z[0] = 1.0f;
            std::array<float, 2048> audio {};
            require(engine.render(z, audio), "ONNX inference failed");
            require(std::abs(audio[256] - std::tanh(1.0f / 16.0f)) < 0.00001f, "ONNX reference output differs");
            require(engine.load(root.getChildFile("invalid.onnx")).isNotEmpty(), "Invalid model accepted");
#else
            require(error.isNotEmpty(), "Unavailable ONNX must report fallback");
#endif
        }
        {
            const auto blocked = root.getChildFile("not-a-directory");
            require(blocked.replaceWithText("test"), "Cannot create failure fixture");
            AetherMatrixProcessor processor(blocked.getChildFile("exports"));
            processor.triggerMasterGeneration();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (processor.isGenerating() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            require(!processor.isGenerating() && !processor.hasGeneratedFiles(), "Failed writes cannot publish files");
        }
        root.deleteRecursively();
        std::cout << "PASS: " << checks << " checks (audio, allocation, timing, state, exports, ONNX, I/O failure)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        root.deleteRecursively();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
