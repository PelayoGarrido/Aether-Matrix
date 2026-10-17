#include "Performance.h"
#include "PianoFixture.h"
#include "PianoSampler.h"
#include "PianoInterpretation.h"
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <iostream>
#include <random>
#include <map>
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
    invalid.notes[0].releaseOffsetUs=invalid.microsAt(480)+1-invalid.microsAt(481);
    rejects([&]{invalid.validate();},"A note that collapses on export was accepted");
    aether::Performance tied;tied.bars=1;tied.notes={{60,80,0,480,0,0},{60,80,480,480,0,0}};
    const auto tie=tied.events();require(tie[1].type==aether::PerformanceEvent::off && tie[2].type==aether::PerformanceEvent::on,"Rearticulation note-off must precede note-on");
}
void interpretationTests()
{
    aether::Performance p;p.bars=2;p.sharps=-1;
    p.notes={{60,80,0,240,0,0},{64,80,480,240,0,0},{60,80,960,240,0,0}};
    p=aether::PianoInterpretation::score(p);auto tokens=aether::PianoInterpretation::tokens(p);
    require(tokens.size()==24 && tokens[0]==65 && tokens[1]==261 && tokens[3]==511 && tokens[9]==761,"Native trained-token units differ from reference");
    require(aether::PianoInterpretation::timingRange(p,0,0)==std::pair{261,262},"First onset must remain fixed");
    require(aether::PianoInterpretation::timingRange(p,1,40)==std::pair{631,812},"Cumulative tempo constraint incorrect");
    tokens[9]+=25;tokens[17]-=25;tokens[10]=178;
    for (size_t i=4;i<8;++i) tokens[i]=5388;
    const auto expressed=aether::PianoInterpretation::decode(p,tokens);
    require(expressed.performance.bpm==120 && expressed.performance.sharps==-1,"Interpretation changed tempo/key");
    require(expressed.performance.notes[1].velocity==45 && expressed.performance.notes[1].onsetOffsetUs==25000,"Learned expression was flattened");
    require(!expressed.performance.pedals.empty() && expressed.performance.pedals[0].value==127,"Learned sustain missing");
    auto bad=tokens;bad[0]+=1;rejects([&]{aether::PianoInterpretation::decode(p,bad);},"Interpretation pitch change accepted");
    bad=tokens;bad[9]+=200;rejects([&]{aether::PianoInterpretation::decode(p,bad);},"Tempo drift accepted");
    bad=tokens;bad[2]=133;rejects([&]{aether::PianoInterpretation::decode(p,bad);},"Zero-velocity model note accepted");
    auto overlap=aether::PianoInterpretation::tokens(p);overlap[3]=1761;
    const auto resolved=aether::PianoInterpretation::decode(p,overlap);
    require(resolved.resolvedReleases==1 && resolved.performance.notes[0].duration==960,"Same-pitch expression resolution differs from exported performance");
    auto existing=p;existing.pedals={{0,0,127}};rejects([&]{aether::PianoInterpretation::score(existing);},"Existing pedal silently replaced");
    existing=p;existing.notes[0].onsetOffsetUs=2000;rejects([&]{aether::PianoInterpretation::score(existing);},"Performance was performed twice");
    rejects([&]{aether::PianoInterpretation::decode(existing,tokens);},"Decode accepted a previously performed score");
    existing=p;std::swap(existing.notes[0],existing.notes[1]);rejects([&]{aether::PianoInterpretation::tokens(existing);},"Unsorted model score accepted");
    existing=p;existing.notes.resize(1);rejects([&]{aether::PianoInterpretation::tokens(existing);},"Single-note context accepted");
    existing=p;existing.notes={{60,80,0,240,0,0},{64,80,0,240,0,0},{67,80,480,240,0,0}};
    require(aether::PianoInterpretation::timingRange(existing,1,0)==std::pair{261,307},"Chord onset envelope differs from reference");
    aether::Performance dense;dense.ppq=1000;dense.bars=4;
    dense.notes={{60,80,0,10,0,0},{62,80,100,10,0,0},{64,80,120,10,0,0},{67,80,120,10,0,0}};
    require(aether::PianoInterpretation::timingRange(dense,1,0).second==367,"A late single-note attack can make the next chord impossible");
    require(aether::PianoInterpretation::timingRange(dense,2,105)==std::pair{261,262},"Dense chord continuation lost feasibility");
    dense.notes={{60,80,0,10,0,0},{60,80,20,10,0,0},{64,80,100,10,0,0}};
    require(aether::PianoInterpretation::timingRange(dense,1,0,1).first==262,"Repeated same-pitch attacks can collapse to the same millisecond");
    dense.notes={{60,80,0,10,0,0},{62,80,200,10,0,0},{64,80,10180,10,0,0},{67,80,10180,10,0,0}};
    require(aether::PianoInterpretation::timingRange(dense,1,0).first==316,"A long gap can strand the next chord outside the interval vocabulary");
    require(aether::PianoInterpretation::timingRange(dense,2,55)==std::pair{5251,5252},"Maximum-gap continuation cannot be sampled");
    // Synthetic boundary stress, not generated music or a listening benchmark.
    std::mt19937 random(17);
    for (int trial=0;trial<32;++trial)
    {
        aether::Performance phrase;phrase.ppq=1000;phrase.bars=4;int64_t tick=0;
        for (int group=0;group<20;++group)
        {
            const int count=1+static_cast<int>(random()%4);
            for (int note=0;note<count;++note) phrase.notes.push_back({50+group%8+note*3,80,tick,2,0,0});
            tick+=2+static_cast<int>(random()%250);
        }
        phrase=aether::PianoInterpretation::score(phrase);const auto original=aether::PianoInterpretation::tokens(phrase);
        for (bool latest:{false,true})
        {
            auto generated=original;int64_t elapsed=0;std::map<int,int64_t> starts;
            for (size_t i=0;i<phrase.notes.size();++i)
            {
                const auto found=starts.find(phrase.notes[i].pitch);const auto minimum=found==starts.end() ? 0 : found->second+1;
                const auto range=aether::PianoInterpretation::timingRange(phrase,i,elapsed,minimum);
                const auto chosen=latest ? range.second-1 : range.first;generated[i*8+1]=chosen;elapsed+=chosen-261;starts[phrase.notes[i].pitch]=elapsed;
            }
            aether::PianoInterpretation::decode(phrase,generated).performance.validate();
        }
    }
    existing=p;existing.bpm=0;rejects([&]{aether::PianoInterpretation::tokens(existing);},"Invalid tempo reached tokenizer arithmetic");
    existing=p;existing.notes[0].duration=6000;rejects([&]{aether::PianoInterpretation::tokens(existing);},"Unbounded note reached tokenizer");
}
using aether_test::Fixture;

void boundaryTests()
{
    // A deliberately discontinuous DC sample exposes clicks that a naturally
    // faded sine fixture hides. Its full body must remain unchanged after 1 ms.
    Fixture f(true);
    for (double rate:{44100.0,48000.0,96000.0})
    {
        aether::PianoSampler reference(f.root,f.manifest,f.layout,rate),polyphonic(f.root,f.manifest,f.layout,rate);
        reference.noteOn(0,{60,127,0,480,0,0});polyphonic.noteOn(0,{60,127,0,480,0,0});
        juce::AudioBuffer<float> a(2,256),b(2,256);
        reference.render(a,256);polyphonic.render(b,256);
        require(std::abs(a.getSample(0,0))<1e-9f,"Nonzero sample start produces a hard note-on step");
        require(a.getSample(0,static_cast<int>(std::ceil(rate*.001))+2)>.199f,"Boundary fade attenuates the sustained sample body");
        polyphonic.noteOn(1,{60,127,0,480,0,0});reference.render(a,256);polyphonic.render(b,256);
        require(std::abs(a.getSample(0,0)-b.getSample(0,0))<1e-7f,"A new attack changes the existing polyphonic mix at its first sample");
        // Follow both attack and physical end-of-file, across irregular chunks.
        aether::PianoSampler ending(f.root,f.manifest,f.layout,rate);ending.pedal(127);ending.noteOn(0,{60,127,0,480,0,0});
        float previous=0;double largest=0;int largestFrame=0;
        const int total=static_cast<int>(rate*2.01);
        for (int offset=0;offset<total;)
        {
            const int count=std::min(137,total-offset);ending.render(a,count);
            for (int i=0;i<count;++i) {const float sample=a.getSample(0,i);const double jump=std::abs(sample-previous);if (jump>largest) {largest=jump;largestFrame=offset+i;}previous=sample;}
            offset+=count;
        }
        std::cout<<"Boundary rate="<<rate<<" maximum step="<<largest<<" at frame="<<largestFrame<<'\n';
        require(largest<.01,"Sample boundary contains an abrupt attack or EOF jump");
        require(std::abs(previous)<1e-7f,"End-of-sample fade leaves a residual signal");
    }
}

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
    try {performanceTests();interpretationTests();boundaryTests();samplerTests();std::cout<<"Performance alignment, trained interpretation codec, piano boundaries, dynamics, pedal, chunk invariance and asset integrity passed\n";}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
    return 0;
}
