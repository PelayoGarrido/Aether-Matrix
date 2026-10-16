#include "PluginProcessor.h"
#include "PluginEditor.h"

AetherMatrixProcessor::AetherMatrixProcessor(juce::File outputDirectory)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("AetherMatrix_Worker"),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()),
      exportDirectory(outputDirectory == juce::File{}
          ? juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("AetherMatrix_Exports")
          : std::move(outputDirectory)),
      fifoStorage(static_cast<size_t>(kFifoCapacity), 0.0f),
      hannWindow(static_cast<size_t>(kRaveWindowSize), 0.0f),
      olaAccumulator(static_cast<size_t>(kRaveWindowSize), 0.0f),
      rtOnnxScratch(8192, 0.0f)
{
    for (auto& s : scopeRing)
        s.store(0.0f, std::memory_order_relaxed);

    for (int i = 0; i < kRaveWindowSize; ++i)
    {
        hannWindow[static_cast<size_t>(i)] = 0.5f * (1.0f - std::cos(
            juce::MathConstants<float>::twoPi * static_cast<float>(i) / static_cast<float>(kRaveWindowSize)));
    }

    constexpr std::array<const char*, 8> ids { "matrixX", "matrixY", "rootKey", "scaleType",
                                             "density", "tension", "contour", "space" };
    for (size_t i = 0; i < ids.size(); ++i)
        parameters[i] = apvts.getRawParameterValue(ids[i]);
    startThread(juce::Thread::Priority::normal);
}

AetherMatrixProcessor::~AetherMatrixProcessor()
{
    signalThreadShouldExit();
    notify();
    stopThread(-1);
}

juce::AudioProcessorValueTreeState::ParameterLayout AetherMatrixProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "matrixX", 1 }, "Style Matrix X",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.50f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "matrixY", 1 }, "Style Matrix Y",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.50f));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "rootKey", 1 }, "Root Key",
        juce::StringArray{ "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 9));

    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{ "scaleType", 1 }, "Scale Mode",
        juce::StringArray{ "Natural Minor", "Dorian", "Phrygian Dom", "Major Lush" }, 0));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "density", 1 }, "Rhythmic Density",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.55f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "tension", 1 }, "Harmonic Tension",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.30f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "contour", 1 }, "Articulation / Contour",
        juce::NormalisableRange<float>(0.05f, 1.0f, 0.001f), 0.50f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{ "space", 1 }, "Acoustic Space",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.38f));

    return { params.begin(), params.end() };
}

void AetherMatrixProcessor::computeStyleWeights(float x, float y, std::array<float, 5>& weights) const noexcept
{
    static constexpr std::array<std::pair<float, float>, 5> anchors {{
        { 0.15f, 0.85f },
        { 0.85f, 0.85f },
        { 0.50f, 0.50f },
        { 0.15f, 0.15f },
        { 0.85f, 0.15f } 
    }};

    float sum = 0.0f;
    for (size_t i = 0; i < 5; ++i)
    {
        float dx = x - anchors[i].first;
        float dy = y - anchors[i].second;
        float distSq = dx * dx + dy * dy + 0.025f;
        weights[i] = 1.0f / (distSq * distSq);
        sum += weights[i];
    }

    if (sum > 0.0f)
    {
        for (float& w : weights)
            w /= sum;
    }
}

std::array<float, 5> AetherMatrixProcessor::getStyleWeights() const noexcept
{
    std::array<float, 5> w {};
    float x = readParameter(0);
    float y = readParameter(1);
    computeStyleWeights(x, y, w);
    return w;
}

juce::String AetherMatrixProcessor::getLoadedModelNameSafe() const
{
    const juce::SpinLock::ScopedLockType sl(metadataLock);
    return loadedModelName;
}

juce::File AetherMatrixProcessor::getGeneratedWavFile() const
{
    const juce::SpinLock::ScopedLockType sl(metadataLock);
    return lastExportedWav;
}

juce::File AetherMatrixProcessor::getGeneratedMidiFile() const
{
    const juce::SpinLock::ScopedLockType sl(metadataLock);
    return lastExportedMidi;
}

void AetherMatrixProcessor::copyVisualScopeData(std::array<float, 256>& dest) const noexcept
{
    int start = scopeWritePos.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 256; ++i)
    {
        size_t idx = (static_cast<size_t>(start) + i) & 255;
        dest[i] = scopeRing[idx].load(std::memory_order_relaxed);
    }
}

bool AetherMatrixProcessor::loadOnnxModel(const juce::File& onnxFile)
{
    if (!onnxFile.hasFileExtension("onnx") || !onnxFile.existsAsFile()
        || onnxFile.getSize() <= 0 || onnxFile.getSize() > 16 * 1024 * 1024)
        return false;
    {
        const juce::ScopedLock lock(modelRequestLock);
        requestedModel = onnxFile;
    }
    modelLoadRequested.store(true, std::memory_order_release);
    notify();
    return true;
}

void AetherMatrixProcessor::triggerMasterGeneration()
{
    bool expected = false;
    if (generationInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        filesReadyToDrag.store(false, std::memory_order_release);
        generationProgress.store(0.0f, std::memory_order_relaxed);
        requestMasterRender.store(true, std::memory_order_release);
        notify();
    }
}

void AetherMatrixProcessor::pushToFifo(const float* data, int numSamples) noexcept
{
    int s1, z1, s2, z2;
    audioFifo.prepareToWrite(numSamples, s1, z1, s2, z2);
    if (z1 > 0) std::copy(data, data + z1, fifoStorage.data() + s1);
    if (z2 > 0) std::copy(data + z1, data + z1 + z2, fifoStorage.data() + s2);
    audioFifo.finishedWrite(z1 + z2);
}

int AetherMatrixProcessor::popFromFifo(float* dest, int numSamples) noexcept
{
    int s1, z1, s2, z2;
    audioFifo.prepareToRead(numSamples, s1, z1, s2, z2);
    if (z1 > 0) std::copy(fifoStorage.data() + s1, fifoStorage.data() + s1 + z1, dest);
    if (z2 > 0) std::copy(fifoStorage.data() + s2, fifoStorage.data() + s2 + z2, dest + z1);
    int total = z1 + z2;
    audioFifo.finishedRead(total);
    return total;
}

void AetherMatrixProcessor::computeNextRaveWindow()
{
    const float x = readParameter(0);
    const float y = readParameter(1);
    const float tension = readParameter(5);
    const float contour = readParameter(6);
    std::array<float, kLatentDim> latent {};
    latent[0] = (x * 2.0f - 1.0f) * 2.0f;
    latent[1] = (y * 2.0f - 1.0f) * 2.0f;
    latent[2] = (tension * 2.0f - 1.0f) * 1.8f;
    latent[3] = (contour * 2.0f - 1.0f) * 1.5f;
    for (size_t d = 4; d < latent.size(); ++d)
        latent[d] = std::sin(static_cast<float>(d) * 1.23f + x * 3.14159f + y * 1.57f) * (0.5f + tension * 0.5f);
    std::array<float, kRaveWindowSize> audio {};
    if (!onnxEngine.render(latent, audio))
    {
        onnxModelLoaded.store(false, std::memory_order_release);
        const juce::SpinLock::ScopedLockType lock(metadataLock);
        loadedModelName = "ONNX inference failed; hybrid synthesis active";
        return;
    }
    for (size_t i = 0; i < audio.size(); ++i)
        olaAccumulator[i] += audio[i] * hannWindow[i];
    pushToFifo(olaAccumulator.data(), kRaveHopSize);
    std::copy(olaAccumulator.begin() + kRaveHopSize, olaAccumulator.end(), olaAccumulator.begin());
    std::fill(olaAccumulator.begin() + kRaveHopSize, olaAccumulator.end(), 0.0f);
}

float AetherMatrixProcessor::synthesizeMultiStyleSample(
    double sampleRate,
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
    juce::Random& localRng) noexcept
{
    const float wHipHop    = w[0];
    const float wClassical = w[1];
    const float wPop       = w[2];
    const float wElectro   = w[3];
    const float wCinema    = w[4];

    double vibrato = 1.0 + std::sin(subPhase * 12.0 * juce::MathConstants<double>::twoPi) * (0.004 * (wClassical + wCinema));

    leadPhase += (static_cast<double>(leadFreq) * vibrato) / sampleRate;
    if (leadPhase >= 1.0) leadPhase -= 1.0;

    subPhase += static_cast<double>(bassFreq) / sampleRate;
    if (subPhase >= 1.0) subPhase -= 1.0;

    chordPhase1 += (static_cast<double>(bassFreq) * 2.0) / sampleRate;
    if (chordPhase1 >= 1.0) chordPhase1 -= 1.0;

    chordPhase2 += (static_cast<double>(bassFreq) * 2.9966) / sampleRate;
    if (chordPhase2 >= 1.0) chordPhase2 -= 1.0;

    const auto sine = [](double phase) { return static_cast<float>(std::sin(phase * juce::MathConstants<double>::twoPi)); };
    float subWave = std::tanh(sine(subPhase) * (1.4f + 2.2f * wHipHop + 1.8f * wElectro)) * bassEnv;

    float chordWave = (sine(chordPhase1)
                     + 0.7f * sine(chordPhase2)
                     + 0.35f * sine(chordPhase1 * 2.0)) * 0.35f * chordEnv;

    float pianoPartials = sine(leadPhase)
                        + 0.45f * sine(leadPhase * 2.0) * leadEnv
                        + 0.20f * sine(leadPhase * 3.0) * leadEnv;

    float sawCore = 2.0f * static_cast<float>(leadPhase) - 1.0f;
    float violinBowed = std::tanh(sawCore * (1.8f + 2.5f * tension))
                      + (localRng.nextFloat() * 2.0f - 1.0f) * 0.025f;

    float electroLead = (sawCore * 0.6f + ((leadPhase < (0.3 + 0.4 * tension)) ? 0.5f : -0.5f));

    float leadVoice = (pianoPartials * (wPop + wHipHop * 0.8f + wClassical * 0.4f)
                     + violinBowed   * (wClassical * 1.1f + wCinema * 1.1f)
                     + electroLead   * (wElectro * 1.2f + wPop * 0.3f)) * 0.32f * leadEnv;

    float decayBase = 1.0f - ((1.0f - contour) * 0.0010f + 0.00004f);
    leadEnv  *= (wClassical + wCinema > 0.5f) ? (0.99985f) : decayBase;
    bassEnv  *= (0.9996f + 0.0003f * contour);
    chordEnv *= (0.9997f + 0.0002f * contour);

    float mixed = subWave * (0.55f + 0.35f * (wHipHop + wElectro + wCinema))
                + chordWave * (0.50f + 0.35f * (wPop + wCinema + wHipHop))
                + leadVoice * 0.75f;

    return std::tanh(mixed * 0.95f);
}

void AetherMatrixProcessor::renderOfflineMasterFiles()
{
    generationProgress.store(0.05f, std::memory_order_relaxed);

    const double exportSr = juce::jlimit(22050.0, 192000.0, exportSampleRate.load(std::memory_order_relaxed));
    const double bpm = juce::jlimit(40.0, 300.0, static_cast<double>(currentHostBpm.load(std::memory_order_relaxed)));
    const float x = readParameter(0);
    const float y = readParameter(1);
    const int rootKey = static_cast<int>(readParameter(2));
    const int scaleType = static_cast<int>(readParameter(3));
    const float density = readParameter(4);
    const float tension = readParameter(5);
    const float contour = readParameter(6);
    const float space   = readParameter(7);

    std::array<float, 5> w {};
    computeStyleWeights(x, y, w);

    constexpr int totalSixteenths = 64;
    const double secondsPerBeat = 60.0 / bpm;
    const double secondsPerSixteenth = secondsPerBeat * 0.25;
    const int samplesPerSixteenth = static_cast<int>( std::round(exportSr * secondsPerSixteenth) );
    const int totalSamples = samplesPerSixteenth * totalSixteenths;

    juce::AudioBuffer<float> masterBuffer(2, totalSamples);
    masterBuffer.clear();

    static constexpr std::array<std::array<int, 7>, 4> scales {{
        { 0, 2, 3, 5, 7, 8, 10 },
        { 0, 2, 3, 5, 7, 9, 10 },
        { 0, 1, 4, 5, 7, 8, 10 },
        { 0, 2, 4, 5, 7, 9, 11 } 
    }};
    const auto& scale = scales[static_cast<size_t>(juce::jlimit(0, 3, scaleType))];

    static constexpr std::array<int, 4> progDegrees = { 0, 5, 2, 6 };

    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote(960);

    juce::MidiMessageSequence tempoTrack;
    juce::MidiMessageSequence chordTrack;
    juce::MidiMessageSequence bassTrack;
    juce::MidiMessageSequence leadTrack;

    int microsecondsPerQuarter = static_cast<int>(std::round(60000000.0 / bpm));
    tempoTrack.addEvent(juce::MidiMessage::tempoMetaEvent(microsecondsPerQuarter), 0.0);

    double lPhase = 0.0, sPhase = 0.0, cPhase1 = 0.0, cPhase2 = 0.0;
    float lEnv = 0.0f, bEnv = 0.0f, cEnv = 0.0f;
    float lFreq = 220.0f, bFreq = 110.0f;
    juce::Random offlineRng(42);

    for (int step = 0; step < totalSixteenths; ++step)
    {
        if (threadShouldExit())
            return;
        int bar = step / 16;
        int stepInBar = step % 16;
        int deg = progDegrees[static_cast<size_t>(bar % 4)];

        int bassMidi = 36 + rootKey + scale[static_cast<size_t>(deg % 7)];
        int chordRootMidi = 60 + rootKey + scale[static_cast<size_t>(deg % 7)];
        int chordThirdMidi = 60 + rootKey + scale[static_cast<size_t>((deg + 2) % 7)] + ((deg + 2) / 7) * 12;
        int chordFifthMidi = 60 + rootKey + scale[static_cast<size_t>((deg + 4) % 7)] + ((deg + 4) / 7) * 12;

        double startTick = step * 240.0;

        if (stepInBar == 0 || (stepInBar == 10 && (w[0] + w[2] > 0.4f)))
        {
            double durTicks = (stepInBar == 0) ? 2400.0 : 1200.0;
            chordTrack.addEvent(juce::MidiMessage::noteOn(1, chordRootMidi, (juce::uint8)90), startTick);
            chordTrack.addEvent(juce::MidiMessage::noteOff(1, chordRootMidi), startTick + durTicks);
            chordTrack.addEvent(juce::MidiMessage::noteOn(1, chordThirdMidi, (juce::uint8)85), startTick);
            chordTrack.addEvent(juce::MidiMessage::noteOff(1, chordThirdMidi), startTick + durTicks);
            chordTrack.addEvent(juce::MidiMessage::noteOn(1, chordFifthMidi, (juce::uint8)88), startTick);
            chordTrack.addEvent(juce::MidiMessage::noteOff(1, chordFifthMidi), startTick + durTicks);
            cEnv = 0.9f;
        }

        bool triggerBass = (stepInBar == 0)
                        || (w[0] > 0.25f && (stepInBar == 6 || stepInBar == 10 || stepInBar == 14))
                        || (w[3] > 0.25f && (stepInBar % 2 == 0));
        if (triggerBass)
        {
            bFreq = static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(bassMidi));
            bEnv = 1.0f;
            bassTrack.addEvent(juce::MidiMessage::noteOn(2, bassMidi, (juce::uint8)110), startTick);
            bassTrack.addEvent(juce::MidiMessage::noteOff(2, bassMidi), startTick + 440.0);
        }

        bool triggerLead = (stepInBar % 4 == 0)
                        || (density > 0.35f && (stepInBar % 2 == 0))
                        || (density > 0.70f && offlineRng.nextFloat() < density);
        if (triggerLead)
        {
            int melDegree = (deg + stepInBar + (offlineRng.nextFloat() < tension ? 2 : 0)) % 7;
            int octaveShift = (y > 0.6f) ? 12 : 0;
            int leadMidi = 60 + rootKey + octaveShift + scale[static_cast<size_t>(melDegree)];
            lFreq = static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(leadMidi));
            lEnv = 0.75f + 0.25f * y;

            juce::uint8 vel = static_cast<juce::uint8>(juce::jlimit(50, 124, static_cast<int>(70.0f + y * 50.0f)));
            leadTrack.addEvent(juce::MidiMessage::noteOn(3, leadMidi, vel), startTick);
            leadTrack.addEvent(juce::MidiMessage::noteOff(3, leadMidi), startTick + 220.0);
        }

        int sampleStart = step * samplesPerSixteenth;
        auto* left  = masterBuffer.getWritePointer(0);
        auto* right = masterBuffer.getWritePointer(1);

        for (int i = 0; i < samplesPerSixteenth; ++i)
        {
            float s = synthesizeMultiStyleSample(
                exportSr, lPhase, sPhase, cPhase1, cPhase2,
                lFreq, bFreq, lEnv, bEnv, cEnv, w, tension, contour, offlineRng);

            left[sampleStart + i]  = s;
            right[sampleStart + i] = s;
        }

        generationProgress.store(0.1f + 0.75f * (static_cast<float>(step) / static_cast<float>(totalSixteenths)),
                                 std::memory_order_relaxed);
    }

    juce::dsp::Reverb offlineReverb;
    juce::dsp::ProcessSpec spec { exportSr, 1024, 2 };
    offlineReverb.prepare(spec);
    juce::dsp::Reverb::Parameters revParams;
    revParams.roomSize = 0.35f + space * 0.60f;
    revParams.wetLevel = space * 0.45f;
    revParams.dryLevel = 1.0f - space * 0.18f;
    revParams.width    = 1.0f;
    offlineReverb.setParameters(revParams);

    juce::dsp::AudioBlock<float> block(masterBuffer);
    offlineReverb.process(juce::dsp::ProcessContextReplacing<float>(block));

    const auto& exportDir = exportDirectory;
    if (exportDir.createDirectory().failed())
        throw std::runtime_error("Cannot create export directory");

    const auto id = juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S_") + juce::Uuid().toString();
    const auto wavFile = exportDir.getChildFile("Aether_Master_" + id + ".wav");
    const auto midiFileOut = exportDir.getChildFile("Aether_MidiStems_" + id + ".mid");
    juce::TemporaryFile temporaryWav(wavFile), temporaryMidi(midiFileOut);
    {
        auto stream = temporaryWav.getFile().createOutputStream();
        if (!stream || stream->failedToOpen())
            throw std::runtime_error("Cannot open WAV export");
        juce::WavAudioFormat format;
        const auto options = juce::AudioFormatWriterOptions()
            .withSampleRate(exportSr).withNumChannels(2).withBitsPerSample(24);
        std::unique_ptr<juce::OutputStream> output = std::move(stream);
        auto writer = format.createWriterFor(output, options);
        if (!writer || !writer->writeFromAudioSampleBuffer(masterBuffer, 0, totalSamples))
            throw std::runtime_error("Cannot write WAV export");
        writer->flush();
    }
    chordTrack.updateMatchedPairs();
    bassTrack.updateMatchedPairs();
    leadTrack.updateMatchedPairs();
    tempoTrack.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4, 4), 0.0);
    tempoTrack.addEvent(juce::MidiMessage::textMetaEvent(3, "Tempo"), 0.0);
    chordTrack.addEvent(juce::MidiMessage::textMetaEvent(3, "Chords"), 0.0);
    bassTrack.addEvent(juce::MidiMessage::textMetaEvent(3, "Bass"), 0.0);
    leadTrack.addEvent(juce::MidiMessage::textMetaEvent(3, "Lead"), 0.0);
    for (auto* track : { &tempoTrack, &chordTrack, &bassTrack, &leadTrack })
        track->addEvent(juce::MidiMessage::endOfTrack(), totalSixteenths * 240.0);
    midiFile.addTrack(tempoTrack);
    midiFile.addTrack(chordTrack);
    midiFile.addTrack(bassTrack);
    midiFile.addTrack(leadTrack);
    {
        auto stream = temporaryMidi.getFile().createOutputStream();
        if (!stream || !midiFile.writeTo(*stream, 1))
            throw std::runtime_error("Cannot write MIDI export");
        stream->flush();
        if (stream->getStatus().failed())
            throw std::runtime_error("MIDI export failed");
    }
    if (threadShouldExit())
        return;
    if (!temporaryWav.overwriteTargetFileWithTemporary())
        throw std::runtime_error("Cannot commit WAV export");
    if (!temporaryMidi.overwriteTargetFileWithTemporary())
    {
        wavFile.deleteFile();
        throw std::runtime_error("Cannot commit MIDI export");
    }

    {
        const juce::SpinLock::ScopedLockType ml(metadataLock);
        lastExportedWav  = wavFile;
        lastExportedMidi = midiFileOut;
    }

    generationProgress.store(1.0f, std::memory_order_relaxed);
    filesReadyToDrag.store(true, std::memory_order_release);
}

void AetherMatrixProcessor::run()
{
    const auto shared = juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory)
        .getChildFile("AetherAudioLabs/AetherMatrix/rave_decoder.onnx");
    auto loadModel = [this](const juce::File& file)
    {
        onnxModelLoaded.store(false, std::memory_order_release);
        const auto error = onnxEngine.load(file);
        std::fill(olaAccumulator.begin(), olaAccumulator.end(), 0.0f);
        {
            const juce::SpinLock::ScopedLockType lock(metadataLock);
            loadedModelName = error.isNotEmpty() ? error : (file == juce::File{}
                ? "Reference ONNX decoder (not trained RAVE)" : "ONNX: " + file.getFileName());
        }
        modelEpoch.fetch_add(1, std::memory_order_release);
        onnxModelLoaded.store(error.isEmpty(), std::memory_order_release);
    };
    loadModel(shared.existsAsFile() ? shared : juce::File{});
    while (!threadShouldExit())
    {
        if (modelLoadRequested.exchange(false, std::memory_order_acq_rel))
        {
            juce::File file;
            {
                const juce::ScopedLock lock(modelRequestLock);
                file = requestedModel;
            }
            loadModel(file);
        }
        else if (requestMasterRender.exchange(false, std::memory_order_acq_rel))
        {
            try { renderOfflineMasterFiles(); }
            catch (const std::exception&)
            {
                filesReadyToDrag.store(false, std::memory_order_release);
                const juce::SpinLock::ScopedLockType lock(metadataLock);
                loadedModelName = "Export failed. Check free space and Music folder permissions.";
            }
            generationInProgress.store(false, std::memory_order_release);
        }
        else if (audioActive.load(std::memory_order_acquire)
                 && onnxModelLoaded.load(std::memory_order_acquire)
                 && audioFifo.getNumReady() < 4096)
            computeNextRaveWindow();
        else
            wait(10);
    }
}

void AetherMatrixProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = std::isfinite(sampleRate) ? juce::jlimit(22050.0, 192000.0, sampleRate) : 48000.0;
    exportSampleRate.store(currentSampleRate, std::memory_order_relaxed);
    audioActive.store(currentSampleRate == 48000.0, std::memory_order_release);
    notify();
    fallbackStepCounter = 0.0;
    stepCounter = 0;
    rtLeadEnv = rtBassEnv = rtChordEnv = 0.0f;
    lastOnnxSample = 0.0f;
    lastStepIndex = -1.0;

    rtOnnxScratch.assign(static_cast<size_t>(juce::jlimit(8192, 65536, samplesPerBlock)), 0.0f);

    smoothX.reset(currentSampleRate, 0.015);
    smoothY.reset(currentSampleRate, 0.015);
    smoothX.setCurrentAndTargetValue(readParameter(0));
    smoothY.setCurrentAndTargetValue(readParameter(1));
    masterReverb.reset();
    masterFilter.reset();

    juce::dsp::ProcessSpec stereoSpec { currentSampleRate, static_cast<juce::uint32>(std::max(1, samplesPerBlock)), 2 };
    masterReverb.prepare(stereoSpec);

    juce::dsp::ProcessSpec monoSpec { currentSampleRate, static_cast<juce::uint32>(std::max(1, samplesPerBlock)), 1 };
    masterFilter.prepare(monoSpec);
    masterFilter.setType(juce::dsp::StateVariableTPTFilterType::lowpass);
}

void AetherMatrixProcessor::releaseResources()
{
    audioActive.store(false, std::memory_order_release);
}

void AetherMatrixProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || buffer.getNumChannels() == 0)
        return;

    smoothX.setTargetValue(readParameter(0));
    smoothY.setTargetValue(readParameter(1));

    const int rootKey   = static_cast<int>(readParameter(2));
    const int scaleType = static_cast<int>(readParameter(3));
    const float density = readParameter(4);
    const float tension = readParameter(5);
    const float contour = readParameter(6);
    const float space   = readParameter(7);

    double hostBpm = 120.0;
    double hostPpq = -1.0;
    bool hostPlaying = true;

    if (auto* playHead = getPlayHead())
    {
        if (auto pos = playHead->getPosition())
        {
            if (auto b = pos->getBpm(); b && std::isfinite(*b)) hostBpm = juce::jlimit(40.0, 300.0, *b);
            if (auto p = pos->getPpqPosition(); p && std::isfinite(*p)) hostPpq = *p;
            hostPlaying = pos->getIsPlaying();
        }
    }
    currentHostBpm.store(static_cast<float>(hostBpm), std::memory_order_relaxed);

    auto midiIterator = midiMessages.cbegin();
    const auto midiEnd = midiMessages.cend();

    // Only the audio consumer advances the read cursor; neither thread resets the FIFO.
    const auto epoch = modelEpoch.load(std::memory_order_acquire);
    if (audioModelEpoch != epoch)
    {
        audioFifo.finishedRead(audioFifo.getNumReady());
        audioModelEpoch = epoch;
        lastOnnxSample = 0.0f;
    }
    int poppedOnnx = 0;
    const bool useOnnx = onnxModelLoaded.load(std::memory_order_acquire) && currentSampleRate == 48000.0;
    if (useOnnx && static_cast<int>(rtOnnxScratch.size()) >= numSamples)
        poppedOnnx = popFromFifo(rtOnnxScratch.data(), numSamples);

    static constexpr std::array<std::array<int, 7>, 4> scales {{
        { 0, 2, 3, 5, 7, 8, 10 },
        { 0, 2, 3, 5, 7, 9, 10 },
        { 0, 1, 4, 5, 7, 8, 10 },
        { 0, 2, 4, 5, 7, 9, 11 }
    }};
    const auto& activeScale = scales[static_cast<size_t>(juce::jlimit(0, 3, scaleType))];

    double stepsPerBeat = (density < 0.35f) ? 2.0 : (density < 0.75f ? 4.0 : 8.0);
    double samplesPerStep = (currentSampleRate * 60.0) / (hostBpm * stepsPerBeat);

    auto* left  = buffer.getWritePointer(0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
    int scopeIdx = scopeWritePos.load(std::memory_order_relaxed);

    for (int i = 0; i < numSamples; ++i)
    {
        while (midiIterator != midiEnd && (*midiIterator).samplePosition <= i)
        {
            const auto event = *midiIterator++;
           
            if (event.numBytes == 3)
            {
                const auto status = event.data[0] & 0xf0;
                const auto note = event.data[1] & 0x7f;
                const auto velocity = event.data[2] & 0x7f;
                if (status == 0x90 && velocity != 0)
                {
                    rtLeadFreq = std::min(static_cast<float>(currentSampleRate * 0.45), static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(note)));
                    rtBassFreq = rtLeadFreq * 0.5f;
                    rtLeadEnv = static_cast<float>(velocity) / 127.0f;
                    rtBassEnv = rtLeadEnv * 0.6f;
                    rtChordEnv = rtLeadEnv * 0.7f;
                    activeMidiNote.store(note, std::memory_order_relaxed);
                }
                else if (status == 0x80 || (status == 0x90 && velocity == 0)
                         || (status == 0xb0 && (note == 120 || note == 123)))
                    rtLeadEnv = rtBassEnv = rtChordEnv = 0.0f;
            }
        }
        float cx = smoothX.getNextValue();
        float cy = smoothY.getNextValue();

        std::array<float, 5> styleW {};
        computeStyleWeights(cx, cy, styleW);

        bool triggerStep = false;
        if (hostPpq >= 0.0 && hostPlaying)
        {
            double samplePpq = hostPpq + (static_cast<double>(i) / currentSampleRate) * (hostBpm / 60.0);
            double curStep = std::floor(samplePpq * stepsPerBeat);
            if (std::abs(curStep - lastStepIndex) > 0.5)
            {
                lastStepIndex = curStep;
                triggerStep = true;
            }
        }
        else if (hostPlaying)
        {
            fallbackStepCounter += 1.0;
            if (fallbackStepCounter >= samplesPerStep)
            {
                fallbackStepCounter -= samplesPerStep;
                triggerStep = true;
            }
        }

        if (triggerStep)
        {
            stepCounter = (stepCounter + 1) % 112;
            int degree = (stepCounter + (rng.nextFloat() < tension ? 2 : 0)) % 7;
            int octave = (cy > 0.55f) ? 12 : 0;
            int newMidi = juce::jlimit(36, 96, 60 + rootKey + octave + activeScale[static_cast<size_t>(degree)]);
            int bassMidi = juce::jlimit(36, 60, 36 + rootKey + activeScale[static_cast<size_t>((stepCounter / 4) % 7)]);

            rtLeadFreq = static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(newMidi));
            rtBassFreq = static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(bassMidi));

            rtLeadEnv = 0.55f + 0.45f * cy;
            if (stepCounter % 2 == 0) rtBassEnv = 0.85f;
            if (stepCounter % 4 == 0) rtChordEnv = 0.80f;

            activeMidiNote.store(newMidi, std::memory_order_relaxed);
        }

        float synthSample = synthesizeMultiStyleSample(
            currentSampleRate, rtLeadPhase, rtSubPhase, rtChordPhase1, rtChordPhase2,
            rtLeadFreq, rtBassFreq, rtLeadEnv, rtBassEnv, rtChordEnv,
            styleW, tension, contour, rng);

        float cutoffHz = 260.0f * std::pow(32.0f, juce::jlimit(0.0f, 1.0f, cx * 0.65f + cy * 0.35f));
        masterFilter.setCutoffFrequency(juce::jlimit(120.0f, static_cast<float>(currentSampleRate * 0.45), cutoffHz));
        masterFilter.setResonance(0.6f + tension * 1.4f);
        float filtered = masterFilter.processSample(0, synthSample);

        if (useOnnx)
        {
            float neural = (i < poppedOnnx) ? rtOnnxScratch[static_cast<size_t>(i)] : lastOnnxSample * 0.995f;
            lastOnnxSample = neural;
            filtered = std::tanh(neural * rtLeadEnv * 0.65f + filtered * 0.60f);
        }

        left[i] = filtered;
        if (right != nullptr) right[i] = filtered;

        if ((i & 3) == 0)
        {
            scopeRing[static_cast<size_t>(scopeIdx)].store(filtered, std::memory_order_relaxed);
            scopeIdx = (scopeIdx + 1) & 255;
        }
    }

    midiMessages.clear();
    scopeWritePos.store(scopeIdx, std::memory_order_relaxed);

    juce::dsp::Reverb::Parameters revParams;
    revParams.roomSize = space * 0.90f;
    revParams.wetLevel = space * 0.50f;
    revParams.dryLevel = 1.0f - space * 0.20f;
    revParams.width    = 0.90f;
    masterReverb.setParameters(revParams);

    juce::dsp::AudioBlock<float> block(buffer);
    masterReverb.process(juce::dsp::ProcessContextReplacing<float>(block));

    currentRmsLevel.store(buffer.getRMSLevel(0, 0, numSamples), std::memory_order_relaxed);
}

void AetherMatrixProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary(*xml, destData);
}

void AetherMatrixProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0 || sizeInBytes > 65536)
        return;
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
    {
        if (!xml->hasTagName(apvts.state.getType()))
            return;
        const auto candidate = juce::ValueTree::fromXml(*xml);
        auto canonical = apvts.copyState();
        for (int i = 0; i < canonical.getNumChildren(); ++i)
        {
            auto child = canonical.getChild(i);
            const auto id = child.getProperty("id").toString();
            const auto incoming = candidate.getChildWithProperty("id", id);
            if (!incoming.isValid())
                continue;
            const auto value = static_cast<float>(incoming.getProperty("value"));
            const auto* parameter = apvts.getParameter(id);
            if (!std::isfinite(value) || parameter == nullptr
                || value < parameter->getNormalisableRange().start
                || value > parameter->getNormalisableRange().end)
                return;
            child.setProperty("value", value, nullptr);
        }
        apvts.replaceState(canonical);
    }
}

juce::AudioProcessorEditor* AetherMatrixProcessor::createEditor()
{
    return new AetherMatrixEditor(*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AetherMatrixProcessor();
}
