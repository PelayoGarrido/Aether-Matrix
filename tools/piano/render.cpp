#include "../../Source/NativeEntry.h"
#include "Performance.h"
#include "PianoSampler.h"
#include <juce_cryptography/juce_cryptography.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef __APPLE__
 #include <mach/mach.h>
 #include <pthread.h>
 #include <sys/resource.h>
#endif

namespace
{
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
volatile std::sig_atomic_t cancelled=0;
void signalHandler(int) {cancelled=1;}
juce::File file(const char* value) {return juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(value));}
fs::path native(const juce::File& path)
{
#ifdef _WIN32
    return fs::path(path.getFullPathName().toWideCharPointer());
#else
    return fs::path(path.getFullPathName().toStdString());
#endif
}
size_t footprint()
{
#ifdef __APPLE__
    task_vm_info_data_t data {};mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(),TASK_VM_INFO,reinterpret_cast<task_info_t>(&data),&count)==KERN_SUCCESS)
        return static_cast<size_t>(data.phys_footprint);
#endif
    return 0;
}
void writeJson(const juce::File& path,const juce::var& value)
{
    if (!path.replaceWithText(juce::JSON::toString(value,true))) throw std::runtime_error("Could not write job metadata");
}
juce::String hash(const juce::File& file)
{
    juce::FileInputStream stream(file);if (!stream.openedOk()) throw std::runtime_error("Could not hash input");
    return juce::SHA256(stream).toHexString();
}
}
static int runNative(int argc,char** argv)
{
    juce::File output;
    bool owned=false,complete=false;
    try
    {
        if (argc<6 || argc>7) throw std::runtime_error("Usage: AetherPianoRender BANK MANIFEST LAYOUT PERFORMANCE_JSON NEW_JOB_DIRECTORY [44100|48000|96000]");
        const auto rateText=argc==7 ? std::string(argv[6]) : std::string("48000");
        if (rateText!="44100" && rateText!="48000" && rateText!="96000") throw std::runtime_error("Unsupported output sample rate");
        const double rate=std::stoi(rateText);
        const auto source=file(argv[4]);output=file(argv[5]);
        juce::FileInputStream input(source);juce::MemoryBlock inputBytes;
        if (!input.openedOk()) throw std::runtime_error("Cannot read performance input");
        input.readIntoMemoryBlock(inputBytes,1024*1024+1);
        if (inputBytes.getSize()>1024*1024) throw std::runtime_error("Performance input exceeds 1 MiB");
        const auto original=juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(inputBytes.getData()),static_cast<int>(inputBytes.getSize())));
        const auto performance=aether::Performance::fromJson(original);
        const auto events=performance.events();
        const auto sourceHash=juce::SHA256(inputBytes.getData(),inputBytes.getSize()).toHexString();
        const auto endUs=events.back().micros;
        const auto maximumFrames=static_cast<int64_t>(std::ceil((endUs/1000000.0+8)*rate));
        const uint64_t reserve=3ULL*1024*1024*1024;
        const uint64_t diskBudget=static_cast<uint64_t>(maximumFrames)*14+4*1024*1024;
        if (fs::space(native(output.getParentDirectory())).available<reserve+diskBudget)
            throw std::runtime_error("Preserve 3 GiB free plus the bounded render output budget");
        if (!fs::create_directory(native(output))) throw std::runtime_error("Output job directory already exists");
        owned=true;
        std::signal(SIGINT,signalHandler);std::signal(SIGTERM,signalHandler);
#ifdef __APPLE__
        if (setpriority(PRIO_PROCESS,0,10)!=0 || pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0)!=0)
            std::cerr<<"Could not lower render scheduling priority\n";
#endif
        const auto start=Clock::now();
        aether::PianoSampler piano(file(argv[1]),file(argv[2]),file(argv[3]),rate);
        juce::WavAudioFormat format;
        const auto raw=output.getChildFile("mix.float.wav");
        std::unique_ptr<juce::OutputStream> stream=raw.createOutputStream();
        auto options=juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(32)
            .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
        auto writer=format.createWriterFor(stream,options);
        if (!writer) throw std::runtime_error("Could not create float render spool");
        juce::AudioBuffer<float> buffer(2,512);
        size_t next=0,peakFootprint=0,peakCache=0;
        int64_t frame=0;
        double peak=0,tailPeak=0;
        juce::Array<juce::var> trace;
        auto frameAt=[rate](int64_t micros) {return static_cast<int64_t>(std::llround(micros*rate/1000000.0));};
        while (frame<maximumFrames)
        {
            if (cancelled || Clock::now()-start>std::chrono::seconds(120)) throw std::runtime_error("Piano render cancelled or timed out");
            if (fs::space(native(output)).available<reserve+1024*1024) throw std::runtime_error("Piano render disk reserve reached");
            peakFootprint=std::max(peakFootprint,footprint());
            if (peakFootprint>2ULL*1024*1024*1024) throw std::runtime_error("Piano render exceeded 2 GiB process footprint");
            while (next<events.size() && frameAt(events[next].micros)<=frame)
            {
                const auto& event=events[next++];
                if (event.type==aether::PerformanceEvent::on) piano.noteOn(event.note,performance.notes[event.note]);
                else if (event.type==aether::PerformanceEvent::off) piano.noteOff(event.note);
                else piano.pedal(event.value);
                juce::var entry(new juce::DynamicObject);auto* o=entry.getDynamicObject();
                o->setProperty("kind",static_cast<int>(event.type));o->setProperty("note",static_cast<int>(event.note));
                o->setProperty("micros",static_cast<juce::int64>(event.micros));o->setProperty("frame",static_cast<juce::int64>(frame));
                o->setProperty("value",event.value);trace.add(entry);
            }
            auto count=std::min<int64_t>(512,maximumFrames-frame);
            if (next<events.size()) count=std::min(count,frameAt(events[next].micros)-frame);
            if (count<=0) throw std::runtime_error("Piano timeline did not advance");
            piano.render(buffer,static_cast<int>(count));
            const double blockPeak=buffer.getMagnitude(0,static_cast<int>(count));
            if (!std::isfinite(blockPeak)) throw std::runtime_error("Non-finite piano render");
            peak=std::max(peak,blockPeak);tailPeak=blockPeak;
            if (!writer->writeFromAudioSampleBuffer(buffer,0,static_cast<int>(count))) throw std::runtime_error("Piano spool write failed");
            frame+=count;peakCache=std::max(peakCache,piano.cacheBytes());
        }
        writer.reset();
        if (tailPeak>1.0e-5) throw std::runtime_error("Instrument tail remains audible at the render limit");
        const double gain=peak>0.891250938 ? 0.891250938/peak : 1.0;
        std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(raw.createInputStream().release(),true));
        if (!reader || reader->lengthInSamples!=frame) throw std::runtime_error("Float spool verification failed");
        const auto audio=output.getChildFile("instrument.wav");
        stream=audio.createOutputStream();
        options=juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(24)
            .withMetadata(juce::WavAudioFormat::riffInfoArtist,"Alexander Holm / Salamander Grand Piano v3")
            .withMetadata(juce::WavAudioFormat::riffInfoComment,"CC-BY-3.0 samples; native Aether renderer; performance provenance in render.json");
        writer=format.createWriterFor(stream,options);
        if (!writer) throw std::runtime_error("Could not create 24-bit output");
        juce::Random dither(13);
        for (int64_t position=0;position<frame;position+=512)
        {
            if (cancelled) throw std::runtime_error("Piano render cancelled");
            const int count=static_cast<int>(std::min<int64_t>(512,frame-position));
            if (!reader->read(&buffer,0,count,position,true,true)) throw std::runtime_error("Float spool read failed");
            for (int channel=0;channel<2;++channel)
                for (int i=0;i<count;++i)
                    buffer.setSample(channel,i,static_cast<float>(buffer.getSample(channel,i)*gain)+(dither.nextFloat()-dither.nextFloat())/8388608.0f);
            if (!writer->writeFromAudioSampleBuffer(buffer,0,count)) throw std::runtime_error("24-bit output write failed");
        }
        writer.reset();reader.reset();
        if (!raw.deleteFile()) throw std::runtime_error("Could not remove this job's temporary spool");
        {
            juce::FileOutputStream midi(output.getChildFile("performance.mid"));
            if (!midi.openedOk() || !performance.midi().writeTo(midi,0)) throw std::runtime_error("Performance MIDI export failed");
            midi.flush();if (midi.getStatus().failed()) throw std::runtime_error("MIDI flush failed");
        }
        writeJson(output.getChildFile("performance.json"),performance.canonical());
        const juce::String credits=juce::String::fromUTF8("Salamander Grand Piano v3 — recordings: Alexander Holm.\nSFZ reconstruction: kinwie. Retuning: Markus Fiedler. Sample-start offsets: Jeff Learman.\nSource: https://github.com/sfzinstruments/SalamanderGrandPiano\nSource revision: 3382bf9496bba2486f5ab0de55a264d1dfc38404\nSamples/mapping: Creative Commons Attribution 3.0 Unported — https://creativecommons.org/licenses/by/3.0/\nChanges: restricted native piano mapping, retuned playback, sample-start alignment, deterministic pedal-noise alternation, 0.5-second same-pitch retrigger fade, 1 ms smooth per-voice start/end boundary fades. No claim of full ARIA/SFZ emulation or endorsement.\nThe performance model and its training data have separate terms. This render grants no additional rights to the composition or AI output.\n");
        if (!output.getChildFile("CREDITS.txt").replaceWithText(credits)) throw std::runtime_error("Could not retain instrument attribution");
        auto model=original["candidate"].toString(),revision=original["revision"].toString();
        if (model.length()>128 || model.startsWithChar('/') || model.contains("..") || !model.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._/"))
            model="external_performance";
        if (!revision.containsOnly("0123456789abcdef") || revision.length()>64) revision={};
        juce::var report(new juce::DynamicObject);auto* o=report.getDynamicObject();
        o->setProperty("schema",1);o->setProperty("completed",true);o->setProperty("renderer","Aether recorded piano, offline helper");
        o->setProperty("neural_waveform_generation",false);o->setProperty("source_model",model);o->setProperty("source_model_revision",revision);
        o->setProperty("source_performance_sha256",sourceHash);o->setProperty("source_bank_revision","3382bf9496bba2486f5ab0de55a264d1dfc38404");
        o->setProperty("instrument_license","CC-BY-3.0");o->setProperty("performance_commercial_clearance","not_established");
        o->setProperty("voice_boundary_fade_ms",1.0);o->setProperty("voice_boundary_fade_curve","cubic smoothstep; per voice, no mix ducking");
        o->setProperty("sample_rate",rate);o->setProperty("bits_per_sample",24);o->setProperty("frames",static_cast<juce::int64>(frame));
        o->setProperty("raw_sample_peak",peak);o->setProperty("output_gain",gain);o->setProperty("sample_peak_after_gain",peak*gain);
        o->setProperty("tail_sample_peak",tailPeak);o->setProperty("peak_voices",static_cast<int>(piano.peakVoices()));
        o->setProperty("peak_sample_cache_bytes",static_cast<juce::int64>(peakCache));o->setProperty("sampled_peak_footprint_bytes",static_cast<juce::int64>(peakFootprint));
        o->setProperty("render_seconds",std::chrono::duration<double>(Clock::now()-start).count());o->setProperty("events",trace);
        o->setProperty("midi_sha256",hash(output.getChildFile("performance.mid")));o->setProperty("audio_sha256",hash(audio));
        // This completion record is written last; consumers must require and
        // validate it before treating a directory as a finished performance.
        writeJson(output.getChildFile("render.json"),report);complete=true;
        std::cout<<juce::JSON::toString(report,true).toStdString()<<'\n';
    }
    catch (const std::exception& error)
    {
        if (owned && !complete)
        {
            for (const char* name: {"mix.float.wav","instrument.wav","performance.mid","performance.json","CREDITS.txt","render.json"})
                output.getChildFile(name).deleteFile();
            output.deleteFile(); // Only succeeds for an empty directory; preserves unexpected user files.
        }
        std::cerr<<error.what()<<'\n';return 1;
    }
    return 0;
}

AETHER_NATIVE_ENTRY(runNative)
