#include "Performance.h"
#include "PianoSampler.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition,const char* message) {if (!condition) throw std::runtime_error(message);}
template<class F> void rejects(F action,const char* message)
{bool failed=false;try {action();} catch (const std::exception&) {failed=true;}require(failed,message);}
void performanceTests()
{
    aether::Performance p;p.bpm=137;p.bars=2;
    p.notes={{60,80,480,240,12345,-3456},{64,90,480,480,-2222,1111},{60,75,960,240,0,0}};
    p.pedals={{600,1000,127},{1200,-1000,0}};
    p.validate();
    const auto roundtrip=aether::Performance::fromJson(p.canonical());
    require(juce::JSON::toString(roundtrip.canonical())==juce::JSON::toString(p.canonical()),"Expressive JSON roundtrip changed events");
    juce::MemoryOutputStream bytes;require(p.midi().writeTo(bytes,0),"MIDI write failed");
    juce::MemoryInputStream input(bytes.getData(),bytes.getDataSize(),false);juce::MidiFile midi;
    require(midi.readFrom(input),"MIDI read failed");require(midi.getTimeFormat()==9600,"MIDI precision lost");
    midi.convertTimestampTicksToSeconds();
    const auto events=p.events();size_t next=0;
    const auto* track=midi.getTrack(0);
    for (int i=0;i<track->getNumEvents();++i)
    {
        const auto& message=track->getEventPointer(i)->message;
        if (!(message.isNoteOnOrOff() || message.isControllerOfType(64))) continue;
        require(next<events.size(),"Extra exported MIDI event");const auto& e=events[next++];
        require(std::abs(message.getTimeStamp()*1e6-e.micros)<=60000000.0/(p.bpm*9600)+1,"Audio/MIDI event alignment exceeds one tick");
        require(e.type==aether::PerformanceEvent::on ? message.isNoteOn() : e.type==aether::PerformanceEvent::off ? message.isNoteOff() : message.isControllerOfType(64),"MIDI event order differs from audio timeline");
        if (message.isNoteOnOrOff()) require(message.getNoteNumber()==p.notes[e.note].pitch,"Pitch changed on export");
        else require(message.getControllerValue()==e.value,"Pedal changed on export");
    }
    require(next==events.size(),"Missing exported event");
    auto invalid=p;invalid.notes.push_back(p.notes[0]);rejects([&]{invalid.validate();},"Ambiguous same-pitch overlap accepted");
    invalid=p;invalid.notes[0].onsetOffsetUs=-500001;rejects([&]{invalid.validate();},"Unbounded expression accepted");
    auto json=p.canonical();json.getDynamicObject()->setProperty("bpm",137.5);rejects([&]{aether::Performance::fromJson(json);},"Fractional contract integer accepted");
    invalid=p;invalid.notes[0].duration=100000;rejects([&]{invalid.validate();},"Out-of-bounds duration accepted");
    invalid=p;invalid.pedals[0].value=128;rejects([&]{invalid.validate();},"Invalid pedal accepted");
    invalid=p;invalid.notes[0].duration=1;invalid.notes[0].onsetOffsetUs=0;
    invalid.notes[0].releaseOffsetUs=invalid.microsAt(480)+500-invalid.microsAt(481);
    rejects([&]{invalid.validate();},"Sub-millisecond note could collapse on MIDI export");
    aether::Performance tied;tied.bars=1;tied.notes={{60,80,0,480,0,0},{60,80,480,480,0,0}};
    const auto tie=tied.events();require(tie[1].type==aether::PerformanceEvent::off && tie[2].type==aether::PerformanceEvent::on,"Rearticulation note-off must precede note-on");
}
struct Fixture
{
    juce::File root=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("aether-piano-tests","",false);
    juce::File manifest,layout;
    Fixture()
    {
        require(root.getChildFile("Samples").createDirectory().wasOk(),"Fixture directory failed");
        manifest=root.getChildFile("manifest.json");layout=root.getChildFile("layout.json");
        auto map=juce::JSON::parse(juce::File(AETHER_PIANO_LAYOUT).loadFileAsString());
        for (auto& region:*map["regions"].getArray())
        {region.getDynamicObject()->setProperty("tune_cents",0);region.getDynamicObject()->setProperty("release_seconds",0.1);juce::Array<juce::var> zeros;for(int i=0;i<16;++i) zeros.add(0);region.getDynamicObject()->setProperty("offset_frames",zeros);}
        require(layout.replaceWithText(juce::JSON::toString(map)),"Fixture layout failed");
        juce::Array<juce::var> assets;
        for (const char* name:{"C4v8.flac","C4v16.flac","rel40.flac","harmLC4.flac","harmV3C4.flac","pedalD1.flac","pedalD2.flac","pedalU1.flac","pedalU2.flac"})
        {
            const bool main=juce::String(name).startsWith("C4v");
            juce::AudioBuffer<float> samples(2,96000);samples.clear();
            if (main) for (int i=0;i<samples.getNumSamples();++i)
            {const float v=static_cast<float>(0.2*std::sin(i*2*juce::MathConstants<double>::pi*440/48000)*std::min(1.0,i/64.0)*std::min(1.0,(96000-i)/512.0));samples.setSample(0,i,v);samples.setSample(1,i,v*0.9f);}
            const auto file=root.getChildFile("Samples").getChildFile(name);
            std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();juce::FlacAudioFormat format;
            auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
            require(writer!=nullptr && writer->writeFromAudioSampleBuffer(samples,0,samples.getNumSamples()),"Fixture FLAC write failed");writer.reset();
            juce::var entry(new juce::DynamicObject);entry.getDynamicObject()->setProperty("path","Samples/"+juce::String(name));
            entry.getDynamicObject()->setProperty("bytes",file.getSize());juce::FileInputStream data(file);
            entry.getDynamicObject()->setProperty("sha256",juce::SHA256(data).toHexString());assets.add(entry);
        }
        juce::var inventory(new juce::DynamicObject);inventory.getDynamicObject()->setProperty("revision","3382bf9496bba2486f5ab0de55a264d1dfc38404");inventory.getDynamicObject()->setProperty("files",assets);
        require(manifest.replaceWithText(juce::JSON::toString(inventory)),"Fixture manifest failed");
    }
    ~Fixture(){root.deleteRecursively();} // Only this test's uniquely-created fixture.
};
void samplerTests()
{
    Fixture f;
    aether::PianoSampler a(f.root,f.manifest,f.layout,48000),b(f.root,f.manifest,f.layout,48000);
    a.noteOn(0,{60,64,0,480,0,0});b.noteOn(0,{60,64,0,480,0,0});
    juce::AudioBuffer<float> whole(2,512),part(2,256),joined(2,512);
    a.render(whole,512);b.render(part,256);joined.copyFrom(0,0,part,0,0,256);joined.copyFrom(1,0,part,1,0,256);
    b.render(part,256);joined.copyFrom(0,256,part,0,0,256);joined.copyFrom(1,256,part,1,0,256);
    for (int c=0;c<2;++c) for(int i=0;i<512;++i) require(std::abs(whole.getSample(c,i)-joined.getSample(c,i))<1e-7,"Render depends on chunk boundaries");
    require(whole.getMagnitude(0,512)>0.02f,"Instrument is silent");
    b.pedal(127);a.noteOff(0);b.noteOff(0);
    for (int i=0;i<40;++i) {a.render(whole,512);b.render(joined,512);}
    require(whole.getMagnitude(0,512)<1e-5f && joined.getMagnitude(0,512)>0.02f,"Sustain pedal does not hold/release correctly");
    b.pedal(0);for (int i=0;i<40;++i)b.render(joined,512);require(joined.getMagnitude(0,512)<1e-5f,"Pedal-up did not damp note");
    aether::PianoSampler loud(f.root,f.manifest,f.layout,48000),soft(f.root,f.manifest,f.layout,48000);
    loud.noteOn(0,{60,127,0,480,0,0});soft.noteOn(0,{60,64,0,480,0,0});loud.render(whole,512);soft.render(joined,512);
    require(whole.getMagnitude(0,512)>joined.getMagnitude(0,512)*2,"Velocity dynamic response lost");
    aether::PianoSampler recapture(f.root,f.manifest,f.layout,48000);
    recapture.noteOn(0,{60,64,0,480,0,0});recapture.pedal(127);recapture.noteOff(0);
    for (int i=0;i<10;++i) {recapture.pedal(0);recapture.render(part,16);recapture.pedal(127);recapture.render(part,16);}
    require(recapture.peakVoices()<=25,"Pedal recapture repeatedly spawned old string-release layers");
    aether::PianoSampler repeated(f.root,f.manifest,f.layout,48000);
    repeated.pedal(127);juce::AudioBuffer<float> phrase(2,4800);
    for (size_t i=0;i<20;++i) {repeated.noteOn(i,{60,64,0,480,0,0});repeated.render(phrase,4800);repeated.noteOff(i);}
    require(repeated.peakVoices()<=28,"Repeated attacks stack unbounded full-length string decays");
    aether::PianoSampler bounded(f.root,f.manifest,f.layout,48000);
    for (size_t i=0;i<256;++i) bounded.noteOn(i,{60,64,0,480,0,0});
    rejects([&]{bounded.noteOn(256,{60,64,0,480,0,0});},"Voice budget silently exceeded");
    require(bounded.peakVoices()==256,"Voice accounting failed");
    const auto changed=f.root.getChildFile("Samples/C4v8.flac");juce::FileOutputStream damage(changed);damage.setPosition(20);damage.writeByte(0);damage.flush();
    aether::PianoSampler tampered(f.root,f.manifest,f.layout,48000);
    rejects([&]{tampered.noteOn(0,{60,64,0,480,0,0});},"Changed sample passed integrity check");
    rejects([&]{aether::PianoSampler invalid(f.root,f.manifest,f.layout,192000);},"Unsupported rate accepted");
}
}
int main()
{
    try {performanceTests();samplerTests();std::cout<<"Performance alignment, bounds, piano dynamics, pedal, chunk invariance and asset integrity passed\n";}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
    return 0;
}
