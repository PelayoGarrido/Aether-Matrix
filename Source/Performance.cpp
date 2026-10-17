#include "Performance.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace aether
{
namespace
{
int64_t integer(const juce::var& object,const char* name,int64_t lo,int64_t hi,int64_t fallback,bool required=false)
{
    const juce::Identifier key(name);
    if (!object.hasProperty(key))
    {
        if (required) throw std::runtime_error(std::string("Missing performance field: ")+name);
        return fallback;
    }
    const auto v=object[key];
    if (!v.isInt() && !v.isInt64()) throw std::runtime_error("Performance numbers must be integers");
    const auto n=static_cast<int64_t>(static_cast<juce::int64>(v));
    if (n<lo || n>hi) throw std::runtime_error(std::string("Performance value outside range: ")+name);
    return n;
}
juce::DynamicObject* object() {return new juce::DynamicObject;}
}
Performance Performance::load(const juce::File& file)
{
    if (!file.existsAsFile() || file.getSize()>1024*1024 || file.getSize()<2)
        throw std::runtime_error("Choose a performance JSON smaller than 1 MiB");
    juce::FileInputStream stream(file);juce::MemoryBlock bytes;
    if (!stream.openedOk()) throw std::runtime_error("Cannot read performance");
    stream.readIntoMemoryBlock(bytes,1024*1024+1);
    if (bytes.getSize()>1024*1024) throw std::runtime_error("Performance exceeds 1 MiB");
    juce::var value;
    if (juce::JSON::parse(juce::String::fromUTF8(static_cast<const char*>(bytes.getData()),static_cast<int>(bytes.getSize())),value).failed())
        throw std::runtime_error("Invalid performance JSON");
    return fromJson(value);
}
Performance Performance::fromJson(const juce::var& value)
{
    if (!value.isObject()) throw std::runtime_error("Invalid performance object");
    integer(value,"schema",1,2,1);
    Performance result;
    result.bpm=static_cast<int>(integer(value,"bpm",40,300,120,true));
    result.bars=static_cast<int>(integer(value,"bars",1,32,8,true));
    result.ppq=static_cast<int>(integer(value,"ticks_per_beat",96,15360,480));
    result.sharps=static_cast<int>(integer(value,"sharps",-7,7,0));
    result.minor=static_cast<int>(integer(value,"minor",0,1,0));
    integer(value,"gm_program",0,0,0);
    const auto* notes=value["notes"].getArray();
    if (notes==nullptr || notes->isEmpty() || notes->size()>512)
        throw std::runtime_error("Performance requires 1 to 512 piano notes");
    const int64_t end=static_cast<int64_t>(result.bars)*4*result.ppq;
    for (const auto& note:*notes)
    {
        if (!note.isObject()) throw std::runtime_error("Invalid note object");
        result.notes.push_back({static_cast<int>(integer(note,"pitch",21,108,60,true)),
            static_cast<int>(integer(note,"velocity",1,127,80,true)),
            integer(note,"tick",0,end,0,true),integer(note,"duration",1,end,480,true),
            integer(note,"onset_offset_us",-500000,500000,0),integer(note,"release_offset_us",-500000,500000,0)});
    }
    if (value.hasProperty("pedals"))
    {
        const auto* pedals=value["pedals"].getArray();
        if (pedals==nullptr || pedals->size()>2048) throw std::runtime_error("Invalid pedal list");
        for (const auto& p:*pedals)
        {
            if (!p.isObject()) throw std::runtime_error("Invalid pedal object");
            result.pedals.push_back({integer(p,"tick",0,end,0,true),integer(p,"offset_us",-500000,500000,0),
                static_cast<int>(integer(p,"value",0,127,0,true))});
        }
    }
    result.validate();
    return result;
}
int64_t Performance::microsAt(int64_t tick,int64_t offset) const
{
    return static_cast<int64_t>(std::llround(static_cast<double>(tick)*60000000.0/(bpm*ppq)))+offset;
}
void Performance::validate() const
{
    if (bpm<40 || bpm>300 || ppq<96 || ppq>15360 || bars<1 || bars>32 || sharps<-7 || sharps>7
        || minor<0 || minor>1 || notes.empty() || notes.size()>512 || pedals.size()>2048)
        throw std::runtime_error("Invalid bounded performance");
    const auto end=static_cast<int64_t>(bars)*4*ppq;
    std::vector<std::tuple<int,int64_t,int64_t>> intervals;
    for (const auto& n:notes)
    {
        if (n.pitch<21 || n.pitch>108 || n.velocity<1 || n.velocity>127 || n.tick<0 || n.tick>=end
            || n.duration<1 || n.duration>end-n.tick || n.onsetOffsetUs<-500000 || n.onsetOffsetUs>500000
            || n.releaseOffsetUs<-500000 || n.releaseOffsetUs>500000)
            throw std::runtime_error("Invalid note range or timeline");
        const auto begin=microsAt(n.tick,n.onsetOffsetUs),finish=microsAt(n.tick+n.duration,n.releaseOffsetUs);
        if (begin<0 || finish<=begin || finish-begin>180000000)
            throw std::runtime_error("Invalid expressive note timing");
        // Validate the actual export clocks instead of an arbitrary minimum.
        // A valid 1 ms learned note can become 0.9375 ms in 9600-PPQ MIDI at
        // 40 BPM. Importing that editable MIDI must not reject our own output.
        const double midiScale=9600.0/(60000000/bpm);
        if (juce::roundToInt(finish*midiScale)<=juce::roundToInt(begin*midiScale))
            throw std::runtime_error("Note collapses at the MIDI export precision");
        for (double rate:{44100.0,48000.0,96000.0})
            if (std::llround(finish*rate/1e6)<=std::llround(begin*rate/1e6))
                throw std::runtime_error("Note collapses at an audio export rate");
        intervals.emplace_back(n.pitch,begin,finish);
    }
    std::sort(intervals.begin(),intervals.end());
    for (size_t i=1;i<intervals.size();++i)
        if (std::get<0>(intervals[i])==std::get<0>(intervals[i-1]) && std::get<1>(intervals[i])<std::get<2>(intervals[i-1]))
            throw std::runtime_error("Overlapping same-pitch MIDI voices need explicit resolution");
    for (const auto& p:pedals)
        if (p.tick<0 || p.tick>end || p.offsetUs<-500000 || p.offsetUs>500000 || p.value<0 || p.value>127 || microsAt(p.tick,p.offsetUs)<0)
            throw std::runtime_error("Invalid pedal event");
}
std::vector<PerformanceEvent> Performance::events() const
{
    validate();
    std::vector<PerformanceEvent> result;
    result.reserve(notes.size()*2+pedals.size()+1);
    for (size_t i=0;i<notes.size();++i)
    {
        const auto& n=notes[i];
        result.push_back({PerformanceEvent::on,microsAt(n.tick,n.onsetOffsetUs),i,0});
        result.push_back({PerformanceEvent::off,microsAt(n.tick+n.duration,n.releaseOffsetUs),i,0});
    }
    for (const auto& p:pedals) result.push_back({PerformanceEvent::pedal,microsAt(p.tick,p.offsetUs),0,p.value});
    // Release a held sustain pedal at the end of the actual expressive timeline.
    int64_t end=microsAt(static_cast<int64_t>(bars)*4*ppq);
    for (const auto& e:result) end=std::max(end,e.micros);
    result.push_back({PerformanceEvent::pedal,end,0,0});
    std::stable_sort(result.begin(),result.end(),[](const auto& a,const auto& b) {
        return a.micros==b.micros ? a.type<b.type : a.micros<b.micros;
    });
    return result;
}
juce::MidiFile Performance::midi() const
{
    validate();
    constexpr int outputPpq=9600;
    juce::MidiMessageSequence track;
    track.addEvent(juce::MidiMessage::tempoMetaEvent(60000000/bpm),0);
    track.addEvent(juce::MidiMessage::timeSignatureMetaEvent(4,4),0);
    track.addEvent(juce::MidiMessage::keySignatureMetaEvent(sharps,minor==1),0);
    track.addEvent(juce::MidiMessage::programChange(1,0),0);
    for (const auto& event:events())
    {
        const auto stamp=event.micros*static_cast<double>(outputPpq)/(60000000/bpm);
        juce::MidiMessage message=juce::MidiMessage::controllerEvent(1,64,event.value);
        if (event.type==PerformanceEvent::on)
        {const auto& n=notes[event.note]; message=juce::MidiMessage::noteOn(1,n.pitch,static_cast<juce::uint8>(n.velocity));}
        else if (event.type==PerformanceEvent::off) message=juce::MidiMessage::noteOff(1,notes[event.note].pitch);
        track.addEvent(message,stamp);
    }
    juce::MidiFile result; result.setTicksPerQuarterNote(outputPpq); result.addTrack(track); return result;
}
juce::var Performance::canonical() const
{
    validate(); juce::var result(object()); auto* o=result.getDynamicObject();
    o->setProperty("schema",2); o->setProperty("bpm",bpm); o->setProperty("bars",bars);
    o->setProperty("ticks_per_beat",ppq); o->setProperty("sharps",sharps); o->setProperty("minor",minor); o->setProperty("gm_program",0);
    juce::Array<juce::var> ns,ps;
    for (const auto& n:notes)
    {
        juce::var v(object());auto* a=v.getDynamicObject();
        a->setProperty("tick",static_cast<juce::int64>(n.tick));a->setProperty("duration",static_cast<juce::int64>(n.duration));a->setProperty("pitch",n.pitch);a->setProperty("velocity",n.velocity);
        a->setProperty("onset_offset_us",static_cast<juce::int64>(n.onsetOffsetUs));a->setProperty("release_offset_us",static_cast<juce::int64>(n.releaseOffsetUs));ns.add(v);
    }
    for (const auto& p:pedals)
    {juce::var v(object());auto* a=v.getDynamicObject();a->setProperty("tick",static_cast<juce::int64>(p.tick));a->setProperty("offset_us",static_cast<juce::int64>(p.offsetUs));a->setProperty("value",p.value);ps.add(v);}
    o->setProperty("notes",ns);o->setProperty("pedals",ps); return result;
}
}
