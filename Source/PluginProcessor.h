#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "OnnxEngine.h"
#include <atomic>
#include <array>
#include <vector>
#include <string>
#include <cmath>

class AetherMatrixProcessor : public juce::AudioProcessor,
                              private juce::Thread
{
public:
    explicit AetherMatrixProcessor(juce::File exportDirectory = {});
    ~AetherMatrixProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        return layout.getMainInputChannelSet().isDisabled()
            && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Aether Matrix"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 2.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    bool loadOnnxModel(const juce::File& onnxFile);
    juce::String getLoadedModelNameSafe() const;

    void triggerMasterGeneration();
    bool isGenerating() const noexcept { return generationInProgress.load(std::memory_order_acquire); }
    bool hasGeneratedFiles() const noexcept { return filesReadyToDrag.load(std::memory_order_acquire); }
    float getGenerationProgress() const noexcept { return generationProgress.load(std::memory_order_acquire); }

    juce::File getGeneratedWavFile() const;
    juce::File getGeneratedMidiFile() const;

    void copyVisualScopeData(std::array<float, 256>& dest) const noexcept;
    std::array<float, 5> getStyleWeights() const noexcept;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float> currentRmsLevel { 0.0f };
    std::atomic<float> currentHostBpm  { 120.0f };
    std::atomic<int>   activeMidiNote  { 60 };
    std::atomic<bool>  onnxModelLoaded { false };

private:
    float readParameter(size_t index) const noexcept
    {
        static constexpr std::array<float, 8> defaults { .5f, .5f, 9.0f, 0.0f, .55f, .3f, .5f, .38f };
        static constexpr std::array<float, 8> maxima { 1.0f, 1.0f, 11.0f, 3.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        const auto value = parameters[index]->load(std::memory_order_relaxed);
        return std::isfinite(value) ? juce::jlimit(index == 6 ? .05f : 0.0f, maxima[index], value) : defaults[index];
    }
    void run() override;
    void computeNextRaveWindow();
    void renderOfflineMasterFiles();

    void computeStyleWeights(float x, float y, std::array<float, 5>& weights) const noexcept;
    float synthesizeMultiStyleSample(double sampleRate,
                                     double& leadPhase,
                                     double& subPhase,
                                     double& chordPhase1,
                                     double& chordPhase2,
                                     float leadFreq,
                                     float bassFreq,
                                     float& leadEnv,
                                     float& bassEnv,
                                     float& chordEnv,
                                     const std::array<float, 5>& w,
                                     float tension,
                                     float contour,
                                     juce::Random& localRng) noexcept;

    static constexpr int kRaveWindowSize = 2048;
    static constexpr int kRaveHopSize    = 1024;
    static constexpr int kLatentDim      = 16;
    static constexpr int kFifoCapacity   = 8192;

    const juce::File exportDirectory;
    OnnxEngine onnxEngine;
    std::atomic<unsigned> modelEpoch { 0 };
    unsigned audioModelEpoch = 0;
    juce::CriticalSection modelRequestLock;
    juce::File requestedModel;
    std::atomic<bool> modelLoadRequested { false };
    std::atomic<bool> audioActive { false };
    std::atomic<double> exportSampleRate { 48000.0 };
    std::array<std::atomic<float>*, 8> parameters {};

    mutable juce::SpinLock metadataLock;
    juce::String loadedModelName { "Aether Hybrid Multi-Style Engine" };
    juce::File lastExportedWav;
    juce::File lastExportedMidi;

    std::atomic<bool>   requestMasterRender  { false };
    std::atomic<bool>  generationInProgress { false };
    std::atomic<bool>  filesReadyToDrag     { false };
    std::atomic<float> generationProgress   { 0.0f };

    juce::AbstractFifo audioFifo { kFifoCapacity };
    std::vector<float> fifoStorage;
    std::vector<float> hannWindow;
    std::vector<float> olaAccumulator;
    std::vector<float> rtOnnxScratch;

    mutable std::array<std::atomic<float>, 256> scopeRing {};
    std::atomic<int> scopeWritePos { 0 };

    double currentSampleRate = 48000.0;
    double rtLeadPhase = 0.0, rtSubPhase = 0.0, rtChordPhase1 = 0.0, rtChordPhase2 = 0.0;
    double fallbackStepCounter = 0.0;
    double lastStepIndex = -1.0;

    float rtLeadEnv = 0.0f, rtBassEnv = 0.0f, rtChordEnv = 0.0f;
    float rtLeadFreq = 220.0f, rtBassFreq = 110.0f;
    float lastOnnxSample = 0.0f;

    int stepCounter = 0;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothX;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothY;

    juce::dsp::StateVariableTPTFilter<float> masterFilter;
    juce::dsp::Reverb masterReverb;
    juce::Random rng;

    void pushToFifo(const float* data, int numSamples) noexcept;
    int popFromFifo(float* dest, int numSamples) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AetherMatrixProcessor)
};
