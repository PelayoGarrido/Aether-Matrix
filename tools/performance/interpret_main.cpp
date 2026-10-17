#include "../../Source/NativeEntry.h"
#include "PianoInterpretation.h"
#include "NativeJobSupport.h"
#include <juce_cryptography/juce_cryptography.h>
#include <charconv>
#include <iostream>

static int runNative(int argc,char** argv)
{
    juce::File output;bool owned=false;
    try
    {
        using namespace aether::job;
        if (argc!=5 && argc!=6) throw std::runtime_error("Usage: AetherPianistPerform WEIGHT_DIRECTORY SCORE_JSON NEW_JOB SEED [--quiet]");
        const bool quiet=argc==6 && std::string(argv[5])=="--quiet";
        if (argc==6 && !quiet) throw std::runtime_error("Unknown interpretation option");
        uint32_t seed=0;const std::string seedText(argv[4]);
        const auto parsed=std::from_chars(seedText.data(),seedText.data()+seedText.size(),seed);
        if (parsed.ec!=std::errc{} || parsed.ptr!=seedText.data()+seedText.size()) throw std::runtime_error("Seed must be a uint32 integer");
        const auto source=file(argv[2]);output=file(argv[3]);
        juce::FileInputStream input(source);juce::MemoryBlock bytes;
        if (!input.openedOk()) throw std::runtime_error("Cannot read piano score");
        input.readIntoMemoryBlock(bytes,1024*1024+1);
        if (bytes.getSize()>1024*1024) throw std::runtime_error("Piano score exceeds 1 MiB");
        const auto value=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),static_cast<int>(bytes.getSize())));
        const auto score=aether::PianoInterpretation::score(aether::Performance::fromJson(value));
        // Reject unsupported phrases before loading a 135M-parameter model.
        aether::PianoInterpretation::tokens(score);
        if (std::filesystem::space(native(output.getParentDirectory())).available<3ULL*1024*1024*1024+4*1024*1024) throw std::runtime_error("Preserve 3 GiB plus bounded interpretation outputs");
        if (!std::filesystem::create_directory(native(output))) throw std::runtime_error("Choose a new interpretation job directory");
        owned=true;std::filesystem::permissions(native(output),std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
        initialise();Budget budget{output};
        juce::InterProcessLock lock("AetherMatrix-Pianist-native-v1");
        if (!lock.enter(0)) throw std::runtime_error("Another native piano interpretation is running");
        const auto start=Clock::now();
        std::unique_ptr<aether::PianistEngine> model;
        std::atomic<aether::PianistEngine*> target{nullptr};
        // Declared after model: stopped/joined before the engine is destroyed.
        std::jthread watchdog([&](std::stop_token stop){
            while (!stop.stop_requested())
            {
                if (!budget.observe())
                    if (auto* engine=target.load(std::memory_order_acquire)) {engine->cancel();return;}
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        model=std::make_unique<aether::PianistEngine>(file(argv[1]));
        target.store(model.get(),std::memory_order_release);budget.check();const auto loaded=Clock::now();
        auto result=aether::PianoInterpretation::generate(*model,score,seed,[&](float progress){
            juce::var status(new juce::DynamicObject);status.getDynamicObject()->setProperty("progress",progress);json(output.getChildFile("progress.json"),status);
        },[&]{budget.check();});
        auto performance=result.performance.canonical();auto* p=performance.getDynamicObject();
        p->setProperty("candidate","yhj137/pianist-transformer-rendering");p->setProperty("revision","8f1568201d1ec035c73939d5a67c5168c0b80221");
        json(output.getChildFile("performance.json"),performance);
        {
            juce::FileOutputStream midi(output.getChildFile("performance.mid"));
            if (!midi.openedOk() || !result.performance.midi().writeTo(midi,0)) throw std::runtime_error("Interpretation MIDI write failed");
            midi.flush();if (midi.getStatus().failed()) throw std::runtime_error("Interpretation MIDI flush failed");
        }
        juce::var tokenData(new juce::DynamicObject);juce::Array<juce::var> original,generated;
        for (auto token:result.scoreTokens) original.add(static_cast<juce::int64>(token));
        for (auto token:result.performanceTokens) generated.add(static_cast<juce::int64>(token));
        tokenData.getDynamicObject()->setProperty("input",original);tokenData.getDynamicObject()->setProperty("output",generated);json(output.getChildFile("tokens.json"),tokenData);
        auto sourceModel=value["candidate"].toString();
        if (sourceModel.isEmpty() || sourceModel.length()>128 || sourceModel.startsWithChar('/') || sourceModel.contains("..") || !sourceModel.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._/")) sourceModel="external_score";
        juce::var report(new juce::DynamicObject);auto* o=report.getDynamicObject();
        o->setProperty("schema",1);o->setProperty("completed",true);o->setProperty("backend","Native ONNX Runtime 1.31.0 CPU, FP32, 2 threads");
        o->setProperty("source_model","yhj137/pianist-transformer-rendering");o->setProperty("source_revision","8f1568201d1ec035c73939d5a67c5168c0b80221");
        o->setProperty("source_score_model",sourceModel);
        auto sourceRevision=value["revision"].toString();
        if ((sourceRevision.length()!=40 && sourceRevision.length()!=64) || !sourceRevision.containsOnly("0123456789abcdef")) sourceRevision="unspecified";
        o->setProperty("source_score_revision",sourceRevision);o->setProperty("score_sha256",juce::SHA256(bytes.getData(),bytes.getSize()).toHexString());
        o->setProperty("seed",static_cast<juce::int64>(seed));o->setProperty("sampler","MT19937 / canonical 53-bit uniform / top-p 0.95; distinct from Torch's sampling stream");
        o->setProperty("load_seconds",std::chrono::duration<double>(loaded-start).count());o->setProperty("generation_seconds",std::chrono::duration<double>(Clock::now()-loaded).count());
        o->setProperty("sampled_peak_footprint_bytes",static_cast<juce::int64>(budget.peak.load(std::memory_order_relaxed)));o->setProperty("note_count",static_cast<int>(score.notes.size()));
        o->setProperty("pedal_events",static_cast<int>(result.performance.pedals.size()));o->setProperty("clipped_note_tails",static_cast<int>(result.clippedTails));o->setProperty("resolved_same_pitch_releases",static_cast<int>(result.resolvedReleases));
        o->setProperty("timing_probability_mass_min",result.minimumTimingMass);o->setProperty("timing_probability_mass_mean",result.meanTimingMass);
        o->setProperty("timing_mass_below_0_01_notes",static_cast<int>(result.lowTimingMassCount));
        o->setProperty("minimum_timing_mass_note_index",static_cast<int>(result.minimumTimingMassNote));
        o->setProperty("timing_mass_threshold_is_diagnostic_not_quality_gate",true);
        o->setProperty("timing_policy","Cumulative 90/45 ms; feasible look-ahead, 4990 ms max interval, 1 ms repeated-pitch separation");
        o->setProperty("pitches_preserved",true);o->setProperty("post_hoc_time_stretch",false);o->setProperty("neural_waveform_generation",false);o->setProperty("commercial_clearance","not_established");o->setProperty("quality_approved",false);
        juce::FileInputStream performed(output.getChildFile("performance.json"));o->setProperty("performance_sha256",juce::SHA256(performed).toHexString());
        budget.check();json(output.getChildFile("interpretation.json"),report);
        if (!quiet) std::cout<<juce::JSON::toString(report,true).toStdString()<<'\n';
    }
    catch (const std::exception& error)
    {
        if (owned)
        {
            juce::var failure(new juce::DynamicObject);failure.getDynamicObject()->setProperty("completed",false);failure.getDynamicObject()->setProperty("error",juce::String::fromUTF8(error.what()));
            output.getChildFile("failure.json").replaceWithText(juce::JSON::toString(failure,true));
        }
        std::cerr<<error.what()<<'\n';return 1;
    }
    return 0;
}

AETHER_NATIVE_ENTRY(runNative)
