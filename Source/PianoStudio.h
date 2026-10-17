#pragma once
#include "Performance.h"
#include "AudioPreview.h"
#include "NativeProcess.h"
#include <juce_events/juce_events.h>
#include <memory>

namespace aether
{
class PianoStudio : private juce::Thread
{
public:
    struct Settings {int bpm=120,bars=8,sharps=0,minor=0,seed=42;};
    struct Status
    {
        bool busy=false;float progress=0;
        juce::String message="Choose the local piano resources to begin";
        juce::File wav,midi,job,resources;
        std::shared_ptr<const Performance> performance;
    };
    PianoStudio(AudioPreview&,juce::File outputs,juce::File helperDirectory=NativeProcess::helpers());
    ~PianoStudio() override;
    bool generate(const juce::File& resources,Settings);
    bool importMidi(const juce::File& resources,const juce::File&,Settings);
    void cancel() {const juce::ScopedLock lock(mutex);if (state.busy) cancelled.store(true,std::memory_order_release);}
    Status status() const;
    void selectResources(const juce::File& file) {const juce::ScopedLock lock(mutex);if (!state.busy) {state.resources=file;if (!state.performance) state.message="Ready to generate piano or render your edited MIDI";}}
    static Performance readMidi(const juce::File&,Settings defaults);
private:
    struct Request {juce::File resources,midi;Settings settings;};
    bool submit(Request);
    void run() override;
    void execute(const Request&);
    void update(const juce::String&,float);
    AudioPreview& preview;
    const juce::File outputs,helpers;
    mutable juce::CriticalSection mutex;
    Status state;Request request;
    std::atomic<bool> queued{false},cancelled{false};
};
}
