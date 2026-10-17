#include "../../Source/NativeEntry.h"
#include "PluginProcessor.h"
#include <juce_cryptography/juce_cryptography.h>
#include <chrono>
#include <iostream>
#include <thread>
namespace
{
struct StoppedHost : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override {PositionInfo info;info.setIsPlaying(false);info.setBpm(90);return info;}
};
}
static int runNative(int argc,char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        if (argc!=4 && argc!=5) throw std::runtime_error("Usage: AetherStudioQualify HELPERS LOCAL_RESOURCES OUTPUT_PARENT [EDITED_MIDI]");
        auto path=[](const char* value){return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(value));};
        AetherMatrixProcessor processor(path(argv[3]),path(argv[1]));StoppedHost host;processor.setPlayHead(&host);
        processor.prepareToPlay(44100,64);processor.setPianoMode(true);
        auto* studio=processor.getPianoStudio();if (!studio) throw std::runtime_error("Piano Studio is disabled in this build");
        aether::PianoStudio::Settings settings;settings.bpm=90;settings.bars=8;settings.sharps=-1;settings.seed=91;
        const bool submitted=argc==5 ? studio->importMidi(path(argv[2]),path(argv[4]),settings) : studio->generate(path(argv[2]),settings);
        if (!submitted) throw std::runtime_error("Studio rejected the qualification request");
        using Clock=std::chrono::steady_clock;const auto started=Clock::now();auto next=started;
        juce::AudioBuffer<float> audio(2,64);juce::MidiBuffer midi;midi.ensureSize(16384);
        std::vector<double> callbackMicros;callbackMicros.reserve(150000);size_t workMisses=0,wakeupMisses=0;double maximumWakeup=0;bool ready=false;
        aether::PianoStudio::Status completed;Clock::time_point readyTime{};
        while (true)
        {
            const auto now=Clock::now();const auto wakeup=std::chrono::duration<double,std::micro>(now-next).count();
            maximumWakeup=std::max(maximumWakeup,wakeup);wakeupMisses+=static_cast<size_t>(wakeup>64e6/44100);
            const auto before=Clock::now();processor.processBlock(audio,midi);const auto work=std::chrono::duration<double,std::micro>(Clock::now()-before).count();
            callbackMicros.push_back(work);workMisses+=static_cast<size_t>(work>64e6/44100);
            if (callbackMicros.size()%64==0)
            {
                const auto state=studio->status();
                if (!state.busy && !ready)
                {
                    if (!state.performance || !state.wav.existsAsFile() || !state.midi.existsAsFile()) throw std::runtime_error(state.message.toStdString());
                    completed=state;ready=true;readyTime=Clock::now();processor.getPianoPreview().play();
                }
                if (ready && Clock::now()-readyTime>std::chrono::seconds(2)) break;
                if (Clock::now()-started>std::chrono::seconds(180)) {studio->cancel();throw std::runtime_error("Studio qualification exceeded 180 seconds");}
            }
            next+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(64.0/44100));
            std::this_thread::sleep_until(next);
        }
        processor.getPianoPreview().stop();
        for (int block=0;block<8;++block) processor.processBlock(audio,midi);
        if (processor.getPianoPreview().isPlaying()) throw std::runtime_error("Studio audition did not stop");
        const auto worst=*std::max_element(callbackMicros.begin(),callbackMicros.end());std::sort(callbackMicros.begin(),callbackMicros.end());
        juce::var report(new juce::DynamicObject);auto* r=report.getDynamicObject();
        r->setProperty("schema",1);r->setProperty("completed",true);r->setProperty("job",completed.job.getFileName());r->setProperty("backend","Native app controller and isolated helpers");
        r->setProperty("fresh_ai_composition",argc==4);r->setProperty("edited_midi_render",argc==5);r->setProperty("python_runtime_used",false);
        r->setProperty("generation_and_render_seconds",std::chrono::duration<double>(readyTime-started).count());
        r->setProperty("callback_rate",44100);r->setProperty("callback_frames",64);r->setProperty("callback_calls",static_cast<juce::int64>(callbackMicros.size()));
        r->setProperty("callback_work_deadline_misses",static_cast<juce::int64>(workMisses));r->setProperty("callback_work_max_us",worst);r->setProperty("callback_work_p99_us",callbackMicros[callbackMicros.size()*99/100]);
        r->setProperty("normal_thread_wakeup_deadline_misses",static_cast<juce::int64>(wakeupMisses));r->setProperty("normal_thread_max_wakeup_lateness_us",maximumWakeup);
        r->setProperty("not_audio_device_or_daw_deadline_qualification",true);r->setProperty("audition_stop_verified",true);r->setProperty("musical_quality_approved",false);r->setProperty("commercial_clearance","not_established");
        if (!completed.job.getChildFile("controller-qualification.json").replaceWithText(juce::JSON::toString(report,true))) throw std::runtime_error("Could not retain controller evidence");
        std::cout<<juce::JSON::toString(report,true).toStdString()<<'\n';processor.releaseResources();
    }
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
    return 0;
}
AETHER_NATIVE_ENTRY(runNative)
