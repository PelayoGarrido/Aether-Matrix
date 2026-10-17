#include "PianoStudio.h"
#include "PianoInterpretation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <map>
#include <cstring>
#include <stdexcept>
#if JUCE_MAC
#include <sys/sysctl.h>
#endif
namespace aether
{
namespace
{
void checkedMidiBytes(const juce::MemoryBlock& bytes)
{
    const auto* data=static_cast<const uint8_t*>(bytes.getData());const size_t size=bytes.getSize();size_t position=0;
    auto take=[&](size_t count) {
        if (count>size-position) throw std::runtime_error("Truncated MIDI file");
        const auto* begin=data+position;position+=count;return begin;
    };
    auto u16=[](const uint8_t* d){return (static_cast<unsigned>(d[0])<<8)|d[1];};
    auto u32=[](const uint8_t* d){return (static_cast<uint32_t>(d[0])<<24)|(static_cast<uint32_t>(d[1])<<16)|(static_cast<uint32_t>(d[2])<<8)|d[3];};
    const auto* header=take(14);
    if (std::memcmp(header,"MThd",4) || u32(header+4)!=6 || u16(header+8)>1 || u16(header+10)<1 || u16(header+10)>64 || u16(header+12)<96 || u16(header+12)>15360)
        throw std::runtime_error("Import bounded type 0/1 MIDI with musical ticks");
    const auto tracks=u16(header+10),ppq=u16(header+12);size_t events=0;
    if (u16(header+8)==0 && tracks!=1) throw std::runtime_error("Type 0 MIDI requires one track");
    for (unsigned track=0;track<tracks;++track)
    {
        const auto* chunk=take(8);if (std::memcmp(chunk,"MTrk",4)) throw std::runtime_error("Invalid MIDI track");
        const size_t length=u32(chunk+4);if (length>size-position) throw std::runtime_error("Truncated MIDI track");
        const size_t end=position+length;uint8_t running=0;uint64_t ticks=0;bool ended=false;
        auto byte=[&]{if (position>=end) throw std::runtime_error("Truncated MIDI event");return data[position++];};
        auto variable=[&] {
            uint32_t value=0;
            for (int i=0;i<4;++i) {const auto b=byte();value=(value<<7)|(b&127);if (b<128) return value;}
            throw std::runtime_error("Invalid MIDI variable integer");
        };
        while (position<end)
        {
            if (++events>8192) throw std::runtime_error("MIDI exceeds 8192 events");
            ticks+=variable();if (ticks>32*4*ppq+(ppq+999)/1000) throw std::runtime_error("MIDI exceeds 32 bars");
            if (position>=end) throw std::runtime_error("Truncated MIDI status");
            uint8_t status=data[position]>=128 ? byte() : running;
            if (status==255)
            {
                running=0;const auto kind=byte();const auto count=variable();
                if (count>end-position || count>65536) throw std::runtime_error("Unbounded MIDI metadata");
                const auto* payload=take(count);
                if ((kind==0x51 && count!=3) || (kind==0x58 && (count!=4 || payload[0]!=4 || payload[1]!=2)) || (kind==0x59 && (count!=2 || payload[1]>1)))
                    throw std::runtime_error("Malformed MIDI tempo, metre or key metadata");
                if (kind==0x2f) {if (count || position!=end) throw std::runtime_error("Invalid MIDI track ending");ended=true;}
            }
            else
            {
                const auto type=status&0xf0;
                if (type!=0x80 && type!=0x90 && type!=0xb0 && type!=0xc0) throw std::runtime_error("MIDI contains unsupported instrument automation");
                running=status;const int count=type==0xc0 ? 1 : 2;
                for (int i=0;i<count;++i) if (byte()>127) throw std::runtime_error("Invalid MIDI data byte");
            }
        }
        if (!ended) throw std::runtime_error("MIDI track has no end event");
    }
    if (position!=size) throw std::runtime_error("Unexpected trailing MIDI data");
}
void save(const juce::File& file,const juce::var& value)
{
    if (!file.replaceWithText(juce::JSON::toString(value,true))) throw std::runtime_error("Could not save piano job metadata");
}
juce::var smallJson(const juce::File& file)
{
    if (file.isSymbolicLink() || !file.existsAsFile() || file.getSize()>1024*1024) throw std::runtime_error("Incomplete or oversized piano job metadata");
    juce::FileInputStream input(file);juce::MemoryBlock bytes;
    if (!input.openedOk()) throw std::runtime_error("Cannot read piano job metadata");
    input.readIntoMemoryBlock(bytes,1024*1024+1);
    if (bytes.getSize()>1024*1024) throw std::runtime_error("Piano job metadata exceeds 1 MiB");
    return juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),static_cast<int>(bytes.getSize())));
}
juce::String digest(const juce::File& file)
{
    const auto size=file.getSize();
    if (file.isSymbolicLink() || size<1 || size>64*1024*1024) throw std::runtime_error("Invalid completed piano output size");
    juce::FileInputStream stream(file);if (!stream.openedOk()) throw std::runtime_error("Cannot verify completed piano output");
    const auto result=juce::SHA256(stream,size).toHexString();
    if (stream.getPosition()!=size || file.getSize()!=size) throw std::runtime_error("Piano output changed during verification");
    return result;
}

}
PianoStudio::PianoStudio(AudioPreview& p,juce::File out,juce::File helperDirectory)
    :juce::Thread("Aether_Piano_Studio"),preview(p),outputs(std::move(out)),helpers(std::move(helperDirectory))
{startThread(juce::Thread::Priority::low);}
PianoStudio::~PianoStudio() {cancel();signalThreadShouldExit();notify();stopThread(-1);}
PianoStudio::Status PianoStudio::status() const {const juce::ScopedLock lock(mutex);return state;}
void PianoStudio::update(const juce::String& text,float progress) {const juce::ScopedLock lock(mutex);state.message=text;state.progress=progress;}
bool PianoStudio::submit(Request next)
{
    const auto& s=next.settings;
    if (s.bpm<40 || s.bpm>300 || s.bars<1 || s.bars>32 || s.sharps<-7 || s.sharps>7 || s.minor<0 || s.minor>1 || s.seed<0) return false;
    const juce::ScopedLock lock(mutex);
    if (state.busy) return false;
    state.resources=next.resources;request=std::move(next);state.busy=true;state.progress=0;state.message="Checking local piano resources";
    cancelled.store(false,std::memory_order_release);queued.store(true,std::memory_order_release);notify();return true;
}
bool PianoStudio::generate(const juce::File& resources,Settings s) {return submit({resources,{},s});}
bool PianoStudio::importMidi(const juce::File& resources,const juce::File& midi,Settings s) {return submit({resources,midi,s});}
void PianoStudio::run()
{
    while (!threadShouldExit())
    {
        preview.collect();
        if (!queued.exchange(false,std::memory_order_acq_rel)) {wait(500);continue;}
        Request next;{const juce::ScopedLock lock(mutex);next=request;}
        try {execute(next);}
        catch (const std::exception& e)
        {
            update(cancelled.load() ? "Piano job cancelled - previous take preserved" : juce::String::fromUTF8(e.what()),0);
            const juce::ScopedLock lock(mutex);state.busy=false;
        }
    }
    preview.collect();
}
Performance PianoStudio::readMidi(const juce::File& file,Settings defaults)
{
    if (defaults.bpm<40 || defaults.bpm>300 || defaults.sharps<-7 || defaults.sharps>7 || defaults.minor<0 || defaults.minor>1)
        throw std::runtime_error("Invalid fallback MIDI tempo or key");
    if (file.isSymbolicLink() || !file.hasFileExtension("mid;midi") || !file.existsAsFile() || file.getSize()>1024*1024) throw std::runtime_error("Choose a MIDI file smaller than 1 MiB");
    juce::FileInputStream source(file);juce::MemoryBlock bytes;
    if (!source.openedOk()) throw std::runtime_error("Cannot read MIDI");
    source.readIntoMemoryBlock(bytes,1024*1024+1);
    if (bytes.getSize()>1024*1024) throw std::runtime_error("MIDI exceeds 1 MiB");
    checkedMidiBytes(bytes);juce::MemoryInputStream stream(bytes,false);juce::MidiFile midi;
    if (!midi.readFrom(stream,false) || midi.getNumTracks()<1 || midi.getNumTracks()>64 || midi.getTimeFormat()<96 || midi.getTimeFormat()>15360)
        throw std::runtime_error("MIDI requires 1–64 tracks and 96–15360 ticks per beat");
    Performance p;p.bpm=defaults.bpm;p.sharps=defaults.sharps;p.minor=defaults.minor;p.ppq=midi.getTimeFormat();
    struct Message {juce::MidiMessage value;};std::vector<Message> messages;int total=0;int64_t maximum=0;int tempo=0,channel=0;bool key=false;
    for (int track=0;track<midi.getNumTracks();++track)
    {
        const auto* sequence=midi.getTrack(track);total+=sequence->getNumEvents();if (total>8192) throw std::runtime_error("MIDI exceeds 8192 events");
        for (int i=0;i<sequence->getNumEvents();++i)
        {
            const auto& m=sequence->getEventPointer(i)->message;const auto time=m.getTimeStamp();
            if (!std::isfinite(time) || time<0 || time>static_cast<double>(32*4*p.ppq+(p.ppq+999)/1000) || std::floor(time)!=time) throw std::runtime_error("MIDI exceeds the 32-bar fixed timeline");
            const auto tick=static_cast<int64_t>(time);maximum=std::max(maximum,tick);
            messages.push_back({m});
        }
    }
    std::stable_sort(messages.begin(),messages.end(),[](const auto& a,const auto& b) {
        const auto aTick=static_cast<int64_t>(a.value.getTimeStamp()),bTick=static_cast<int64_t>(b.value.getTimeStamp());
        if (aTick!=bTick) return aTick<bTick;
        const auto priority=[](const juce::MidiMessage& m){return m.isMetaEvent() ? -1 : m.isNoteOff() ? 0 : m.isController() ? 1 : 2;};return priority(a.value)<priority(b.value);
    });
    const int64_t barTicks=4*p.ppq,margin=(p.ppq+999)/1000;
    const auto nearest=(maximum+barTicks/2)/barTicks;
    p.bars=static_cast<int>(nearest>=1 && std::abs(maximum-nearest*barTicks)<=margin ? nearest : (maximum+barTicks-1)/barTicks);
    p.bars=std::max(1,p.bars);
    if (p.bars>32) throw std::runtime_error("MIDI exceeds 32 bars");
    const int64_t endTick=p.bars*barTicks;
    auto actualMicros=[&](int64_t tick) {
        const double quarter=tempo ? static_cast<double>(tempo) : 60000000.0/p.bpm;
        return static_cast<int64_t>(std::llround(tick*quarter/p.ppq));
    };
    std::map<int,size_t> active;
    for (const auto& entry:messages)
    {
        const auto& m=entry.value;const auto tick=static_cast<int64_t>(m.getTimeStamp());
        if (m.isMetaEvent())
        {
            if (m.isTempoMetaEvent())
            {
                const int micros=juce::roundToInt(m.getTempoSecondsPerQuarterNote()*1e6);
                if (micros<=0) throw std::runtime_error("Invalid MIDI tempo");
                const int bpm=juce::roundToInt(60000000.0/micros);
                if (bpm<40 || bpm>300 || std::abs(micros-60000000/bpm)>1 || (tempo && tempo!=micros) || (!tempo && tick!=0))
                    throw std::runtime_error("MIDI must use one integer tempo, 40–300 BPM");
                tempo=micros;p.bpm=bpm;
            }
            else if (m.isTimeSignatureMetaEvent())
            {int n=0,d=0;m.getTimeSignatureInfo(n,d);if (n!=4 || d!=4) throw std::runtime_error("Piano takes currently require 4/4 MIDI");}
            else if (m.isKeySignatureMetaEvent())
            {
                const int sharps=m.getKeySignatureNumberOfSharpsOrFlats(),minor=m.isKeySignatureMajorKey() ? 0 : 1;
                if (sharps<-7 || sharps>7 || tick!=0 || (key && (sharps!=p.sharps || minor!=p.minor))) throw std::runtime_error("MIDI requires one fixed key signature");
                p.sharps=sharps;p.minor=minor;key=true;
            }
            continue;
        }
        if (!(m.isNoteOnOrOff() || m.isControllerOfType(64) || (m.isProgramChange() && m.getProgramChangeNumber()==0)) || m.getChannel()==10)
            throw std::runtime_error("Import one piano channel with notes and sustain only; unsupported automation would change the render");
        if (channel && m.getChannel()!=channel) throw std::runtime_error("Import one instrument channel");
        channel=m.getChannel();
        if (m.isNoteOn())
        {
            if (active.contains(m.getNoteNumber()) || p.notes.size()>=512) throw std::runtime_error("MIDI has overlapping same-pitch notes or more than 512 notes");
            const auto anchor=std::min(tick,endTick-1);
            active[m.getNoteNumber()]=p.notes.size();p.notes.push_back({m.getNoteNumber(),m.getVelocity(),anchor,0,actualMicros(tick)-p.microsAt(anchor),0});
        }
        else if (m.isNoteOff())
        {
            const auto found=active.find(m.getNoteNumber());if (found==active.end()) throw std::runtime_error("MIDI has an unmatched note-off");
            auto& note=p.notes[found->second];const auto anchor=std::clamp(tick,note.tick+1,endTick);
            note.duration=anchor-note.tick;note.releaseOffsetUs=actualMicros(tick)-p.microsAt(anchor);active.erase(found);
        }
        else if (m.isControllerOfType(64)) {if (p.pedals.size()>=2049) throw std::runtime_error("MIDI exceeds 2048 sustain events plus its end reset");const auto anchor=std::min(tick,endTick);p.pedals.push_back({anchor,actualMicros(tick)-p.microsAt(anchor),m.getControllerValue()});}
    }
    if (!active.empty()) throw std::runtime_error("MIDI has hanging notes");
    // Remove only a reset that the canonical timeline will reproduce at the
    // identical SMF tick. Preserve earlier, musically meaningful pedal release.
    if (!p.pedals.empty() && p.pedals.back().value==0)
    {
        const auto final=p.pedals.back();p.pedals.pop_back();int64_t end=p.microsAt(endTick);
        for (const auto& n:p.notes) end=std::max(end,p.microsAt(n.tick+n.duration,n.releaseOffsetUs));
        for (const auto& pedal:p.pedals) end=std::max(end,p.microsAt(pedal.tick,pedal.offsetUs));
        const double quarter=tempo ? static_cast<double>(tempo) : 60000000.0/p.bpm;
        if (std::llround(p.microsAt(final.tick,final.offsetUs)*p.ppq/quarter)!=std::llround(end*p.ppq/quarter)) p.pedals.push_back(final);
    }
    p.validate();return p;
}
void PianoStudio::execute(const Request& r)
{
    juce::InterProcessLock instanceLock("AetherMatrix-PianoStudio-v1");
    if (!instanceLock.enter(0)) throw std::runtime_error("Another Aether piano job is running; finish or cancel it first");
#if JUCE_MAC
    int nativeArm=0;size_t armSize=sizeof(nativeArm);sysctlbyname("hw.optional.arm64",&nativeArm,&armSize,nullptr,0);
    if (r.midi==juce::File{} && !nativeArm) throw std::runtime_error("Trained piano generation currently requires Apple Silicon on macOS");
#endif
    if (r.resources.isSymbolicLink() || !r.resources.isDirectory()) throw std::runtime_error("Choose the folder containing the local piano models and sample bank");
    const auto resources=helpers.getSiblingFile("Resources").getChildFile("piano");
    const auto manifest=resources.getChildFile("salamander-manifest.json"),layout=resources.getChildFile("salamander-layout.json");
#if JUCE_WINDOWS
    const juce::String extension=".exe";
#else
    const juce::String extension;
#endif
    const auto composer=helpers.getChildFile("AetherMidiEvaluate"+extension),interpreter=helpers.getChildFile("AetherPianistPerform"+extension),renderer=helpers.getChildFile("AetherPianoRender"+extension);
    if (!renderer.existsAsFile() || !manifest.existsAsFile() || !layout.existsAsFile()) throw std::runtime_error("This build does not include the native piano helpers");
    const auto bank=r.resources.getChildFile("salamander-v3"),weights=r.resources.getChildFile("pianist-transformer");
    if (!bank.getChildFile("Samples").isDirectory()) throw std::runtime_error("The verified Salamander piano bank is missing");
    if (outputs.createDirectory().failed() || outputs.isSymbolicLink() || outputs.getBytesFreeOnVolume()<3LL*1024*1024*1024+150*1024*1024)
        throw std::runtime_error("Preserve 3 GiB free plus the piano job output budget");
    const auto job=outputs.getChildFile("Piano-"+juce::Uuid().toString());
    if (job.exists() || job.createDirectory().failed()) throw std::runtime_error("Cannot create exclusive piano job");
#if JUCE_MAC
    std::filesystem::permissions(job.getFullPathName().toStdString(),std::filesystem::perms::owner_all,std::filesystem::perm_options::replace);
#endif
    {const juce::ScopedLock lock(mutex);state.job=job;}
    try
    {
        juce::File performanceFile;
        if (r.midi!=juce::File{})
        {
            update("Reading edited MIDI - keeping its notes and sustain",.08f);
            const auto imported=readMidi(r.midi,r.settings);performanceFile=job.getChildFile("edited-performance.json");save(performanceFile,imported.canonical());
        }
        else
        {
            if (!composer.existsAsFile() || !interpreter.existsAsFile()) throw std::runtime_error("This build does not include trained piano generation");
            update("Verifying the local composition model",.02f);
            const auto model=r.resources.getChildFile("midi-tv2o-medium");
            if (!model.getChildFile("model_base.onnx").existsAsFile() || !model.getChildFile("model_token.onnx").existsAsFile())
                throw std::runtime_error("The pinned composition model is missing");
            update("Composing a piano score locally",.08f);const auto prefix=job.getChildFile("score");
            NativeProcess::run(composer,{model.getFullPathName(),prefix.getFullPathName(),"512",juce::String(r.settings.seed),juce::String(r.settings.bpm),juce::String(r.settings.bars),"0",juce::String(r.settings.sharps),juce::String(r.settings.minor)},job.getChildFile("composition.log"),125,cancelled);
            const auto score=Performance::load(job.getChildFile("score.json"));
            if (score.bpm!=r.settings.bpm || score.bars!=r.settings.bars || score.sharps!=r.settings.sharps || score.minor!=r.settings.minor || score.notes.size()<2)
                throw std::runtime_error("Composition failed the requested musical contract");
            // Reject unsupported model coupling before loading the expressive NN.
            try {PianoInterpretation::tokens(PianoInterpretation::score(score));}
            catch (const std::exception& error)
            {
                juce::var rejection(new juce::DynamicObject);rejection.getDynamicObject()->setProperty("completed",false);
                rejection.getDynamicObject()->setProperty("phase","composition_performance_compatibility");rejection.getDynamicObject()->setProperty("reason",juce::String::fromUTF8(error.what()));
                save(job.getChildFile("rejection.json"),rejection);
                throw std::runtime_error("This performance model cannot interpret the score at this tempo. Try another variation; the score is retained in the job files");
            }
            const auto last=std::max_element(score.notes.begin(),score.notes.end(),[](const auto& a,const auto& b){return a.tick<b.tick;});
            if (last->tick<(score.bars-1)*4*score.ppq) throw std::runtime_error("The model stopped before the final requested bar; this take needs regeneration");
            update("Generating learned dynamics, articulation and pedal",.4f);const auto expression=job.getChildFile("expression");
            NativeProcess::run(interpreter,{weights.getFullPathName(),job.getChildFile("score.json").getFullPathName(),expression.getFullPathName(),juce::String(r.settings.seed),"--quiet"},job.getChildFile("interpretation.log"),155,cancelled,[&] {
                const auto file=expression.getChildFile("progress.json");
                if (file.existsAsFile() && file.getSize()<4096)
                {const auto v=juce::JSON::parse(file.loadFileAsString())["progress"];if (v.isDouble() || v.isInt()) {const float progress=static_cast<float>(v);if (std::isfinite(progress)) update("Generating learned dynamics, articulation and pedal",.4f+.25f*std::clamp(progress,0.0f,1.0f));}}
            });
            const auto report=smallJson(expression.getChildFile("interpretation.json"));performanceFile=expression.getChildFile("performance.json");
            if (report["completed"]!=juce::var(true) || report["performance_sha256"].toString()!=digest(performanceFile)) throw std::runtime_error("Incomplete or inconsistent trained interpretation");
        }
        update("Rendering the recorded piano to 24-bit audio",.7f);const auto audio=job.getChildFile("audio");
        NativeProcess::run(renderer,{bank.getFullPathName(),manifest.getFullPathName(),layout.getFullPathName(),performanceFile.getFullPathName(),audio.getFullPathName(),"48000"},job.getChildFile("render.log"),125,cancelled);
        const auto report=smallJson(audio.getChildFile("render.json"));const auto wav=audio.getChildFile("instrument.wav"),midi=audio.getChildFile("performance.mid");
        if (cancelled.load() || report["completed"]!=juce::var(true) || report["source_performance_sha256"].toString()!=digest(performanceFile)
            || wav.isSymbolicLink() || midi.isSymbolicLink() || wav.getSize()>64*1024*1024 || midi.getSize()>1024*1024
            || report["audio_sha256"].toString()!=digest(wav) || report["midi_sha256"].toString()!=digest(midi)) throw std::runtime_error("Completed take verification failed");
        auto performed=std::make_shared<const Performance>(Performance::load(audio.getChildFile("performance.json")));
        if (juce::JSON::toString(performed->canonical())!=juce::JSON::toString(Performance::load(performanceFile).canonical()))
            throw std::runtime_error("Rendered events differ from the completed interpretation");
        if (cancelled.load()) throw std::runtime_error("Piano job cancelled");
        auto decoded=AudioPreview::decode(wav,&cancelled);
        juce::var complete(new juce::DynamicObject);complete.getDynamicObject()->setProperty("completed",true);complete.getDynamicObject()->setProperty("musical_quality_approved",false);complete.getDynamicObject()->setProperty("commercial_clearance","not_established");save(job.getChildFile("take.part.json"),complete);
        const juce::ScopedLock lock(mutex);
        if (cancelled.load()) throw std::runtime_error("Piano job cancelled");
        std::error_code error;
#if JUCE_WINDOWS
        std::filesystem::create_hard_link(std::filesystem::path(job.getChildFile("take.part.json").getFullPathName().toWideCharPointer()),std::filesystem::path(job.getChildFile("take.json").getFullPathName().toWideCharPointer()),error);
#else
        std::filesystem::create_hard_link(job.getChildFile("take.part.json").getFullPathName().toStdString(),job.getChildFile("take.json").getFullPathName().toStdString(),error);
#endif
        if (error) throw std::runtime_error("Cannot commit the take exclusively on this filesystem");
        job.getChildFile("take.part.json").deleteFile();
        preview.stop();preview.publish(std::move(decoded));state.busy=false;state.wav=wav;state.midi=midi;state.performance=std::move(performed);state.progress=1;
        state.message=r.midi==juce::File{} ? "Piano take ready for review - model quality and commercial rights remain unapproved" : "Edited MIDI rendered - notes and sustain preserved";
    }
    catch (const std::exception& error)
    {
        try
        {
            juce::var failure(new juce::DynamicObject);auto* f=failure.getDynamicObject();f->setProperty("completed",false);f->setProperty("cancelled",cancelled.load());f->setProperty("reason",juce::String::fromUTF8(error.what()));
            save(job.getChildFile("failure.json"),failure);
        }
        catch (...) {juce::Logger::writeToLog("Aether could not retain this job's failure metadata");}
        // Child processes have been reaped. Remove only this job's known large
        // temporary spool; keep diagnostics, unexpected files and earlier takes.
        job.getChildFile("audio/mix.float.wav").deleteFile();job.getChildFile("take.part.json").deleteFile();throw;
    }
}
}
