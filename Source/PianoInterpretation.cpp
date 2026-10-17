#include "PianoInterpretation.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <stdexcept>

namespace aether
{
namespace
{
int64_t roundEven(int64_t numerator,int64_t denominator)
{
    const auto q=numerator/denominator,r=numerator%denominator;
    return q+static_cast<int64_t>(r*2>denominator || (r*2==denominator && q%2));
}
int64_t milliseconds(const Performance& p,int64_t tick) {return roundEven(tick*60000,static_cast<int64_t>(p.bpm)*p.ppq);}
const std::array<std::array<int,2>,8> ranges{{{5,133},{261,5252},{134,261},{262,5261},{5261,5389},{5261,5389},{5261,5389},{5261,5389}}};
using TimingWindows=std::vector<std::pair<int64_t,int64_t>>;
TimingWindows timingWindows(const Performance& p)
{
    if (p.bpm<40 || p.bpm>300 || p.ppq<96 || p.ppq>15360 || p.bars<1 || p.bars>32 || p.notes.size()<2 || p.notes.size()>512)
        throw std::runtime_error("Invalid performance timing context");
    const int64_t endTick=static_cast<int64_t>(p.bars)*4*p.ppq;
    const auto latest=(p.microsAt(endTick)-1000)/1000;
    TimingWindows windows;windows.reserve(p.notes.size());
    for (size_t i=0;i<p.notes.size();++i)
    {
        const auto& n=p.notes[i];
        if (n.tick<0 || n.tick>=endTick || n.pitch<21 || n.pitch>108 || (i && n.tick<p.notes[i-1].tick)) throw std::runtime_error("Invalid sorted timing context");
        const bool chord=(i>0 && p.notes[i-1].tick==n.tick) || (i+1<p.notes.size() && p.notes[i+1].tick==n.tick);
        const int tolerance=i==0 ? 0 : chord ? 45 : 90;
        const auto expected=milliseconds(p,n.tick);
        windows.emplace_back(std::max<int64_t>(0,expected-tolerance),std::min(expected+tolerance,latest));
    }
    std::map<int,size_t> nextPitch;
    // Backward reachability preserves the model's 0..4990 ms interval range
    // and a 1 ms gap between repeated same-pitch attacks. Sampling inside these
    // windows leaves a feasible continuation, including single-note/chord edges.
    for (size_t i=windows.size();i-->0;)
    {
        auto& [low,high]=windows[i];
        if (i+1<windows.size()) {low=std::max(low,windows[i+1].first-4990);high=std::min(high,windows[i+1].second);}
        if (const auto next=nextPitch.find(p.notes[i].pitch);next!=nextPitch.end()) high=std::min(high,windows[next->second].second-1);
        nextPitch[p.notes[i].pitch]=i;
        if (low>high) throw std::runtime_error("Score cannot fit the model's millisecond timing constraints");
    }
    return windows;
}
std::pair<int,int> intervalRange(std::pair<int64_t,int64_t> window,int64_t elapsed,int64_t minimum)
{
    if (elapsed<0 || elapsed>192000 || minimum<0 || minimum>192000) throw std::runtime_error("Invalid cumulative performance time");
    const auto low=std::max<int64_t>(261,261+std::max(window.first,minimum)-elapsed);
    const auto high=std::min<int64_t>(5252,261+window.second-elapsed+1);
    if (low>=high) throw std::runtime_error("Phrase cannot fit the model's tempo envelope");
    return {static_cast<int>(low),static_cast<int>(high)};
}

int sample(const std::array<float,PianistEngine::vocabulary>& logits,int low,int high,std::mt19937& rng)
{
    if (low<0 || high>static_cast<int>(logits.size()) || low>=high) throw std::runtime_error("Invalid learned token mask");
    std::vector<int> ids(static_cast<size_t>(high-low));std::iota(ids.begin(),ids.end(),low);
    std::sort(ids.begin(),ids.end(),[&](int a,int b){
        const auto x=logits[static_cast<size_t>(a)],y=logits[static_cast<size_t>(b)];
        if (x>y) return true;if (x<y) return false;return a<b;
    });
    std::vector<double> probabilities;double total=0;
    for (int id:ids) {const auto p=std::exp(static_cast<double>(logits[static_cast<size_t>(id)]-logits[static_cast<size_t>(ids[0])]));probabilities.push_back(p);total+=p;}
    size_t keep=0;double retained=0;
    do {retained+=probabilities[keep++];} while (keep<ids.size() && retained<0.95*total);
    // Canonical 53-bit uniform draw; independent of STL distribution choices.
    const uint64_t bits=(static_cast<uint64_t>(rng()>>5)<<26)|(rng()>>6);
    const double draw=static_cast<double>(bits)/9007199254740992.0*retained;
    double cumulative=0;
    for (size_t i=0;i<keep;++i) {cumulative+=probabilities[i];if (draw<cumulative) return ids[i];}
    return ids[keep-1];
}
double timingMass(const std::array<float,PianistEngine::vocabulary>& logits,int low,int high)
{
    const auto maximum=*std::max_element(logits.begin()+261,logits.begin()+5252);
    double all=0,allowed=0;
    for (int i=261;i<5252;++i) {const auto p=std::exp(static_cast<double>(logits[static_cast<size_t>(i)]-maximum));all+=p;if (i>=low && i<high) allowed+=p;}
    return allowed/all;
}
}
Performance PianoInterpretation::score(Performance p)
{
    p.validate();
    if (p.notes.size()<2 || !p.pedals.empty()) throw std::runtime_error("Interpret an unperformed score with 2..512 notes and no existing pedal automation");
    for (const auto& n:p.notes)
    {
        if (n.onsetOffsetUs || n.releaseOffsetUs) throw std::runtime_error("Existing expression must be rendered directly, not performed twice");
        if (p.microsAt(n.tick+n.duration)-p.microsAt(n.tick)<1000) throw std::runtime_error("Learned interpretation needs notes of at least one millisecond; render edited MIDI directly");
    }
    std::stable_sort(p.notes.begin(),p.notes.end(),[](const auto& a,const auto& b){return a.tick==b.tick ? a.pitch<b.pitch : a.tick<b.tick;});
    if (p.notes.front().tick==p.notes.back().tick) throw std::runtime_error("Score needs more than one onset");
    return p;
}
std::vector<int64_t> PianoInterpretation::tokens(const Performance& p)
{
    const auto canonical=score(p);
    for (size_t i=0;i<p.notes.size();++i)
        if (p.notes[i].tick!=canonical.notes[i].tick || p.notes[i].pitch!=canonical.notes[i].pitch)
            throw std::runtime_error("Tokenise the sorted unperformed score, not reordered model events");
    timingWindows(p); // Reject infeasible timing before loading the NN.
    std::vector<int64_t> result;int64_t previous=0;
    for (const auto& n:p.notes)
    {
        const auto onset=milliseconds(p,n.tick),duration=milliseconds(p,n.duration);
        if (onset<previous || onset-previous>4990 || duration>4999) throw std::runtime_error("Phrase exceeds the trained timing vocabulary; split it explicitly");
        const std::array<int64_t,8> group{5+n.pitch,261+onset-previous,133+n.velocity,261+std::max<int64_t>(1,duration),5261,5261,5261,5261};
        result.insert(result.end(),group.begin(),group.end());previous=onset;
    }
    return result;
}
std::pair<int,int> PianoInterpretation::timingRange(const Performance& p,size_t i,int64_t elapsedMs,int64_t minimumOnsetMs)
{
    const auto windows=timingWindows(p);
    if (i>=windows.size()) throw std::runtime_error("Invalid performance note index");
    return intervalRange(windows[i],elapsedMs,minimumOnsetMs);
}

PianoInterpretationResult PianoInterpretation::decode(const Performance& p,const std::vector<int64_t>& generated)
{
    auto originalTokens=tokens(p);const auto windows=timingWindows(p);
    if (generated.size()!=p.notes.size()*8) throw std::runtime_error("Incomplete learned performance");
    PianoInterpretationResult result;result.performance=p;auto& output=result.performance;
    std::vector<int64_t> starts,finishes;int64_t elapsed=0;
    const auto endTick=static_cast<int64_t>(p.bars)*4*p.ppq,endUs=p.microsAt(endTick);
    auto release=[&](PerformanceNote& n,int64_t end) {
        const auto tick=std::clamp(roundEven(end*p.bpm*p.ppq,60000000),n.tick+1,endTick);
        n.duration=tick-n.tick;n.releaseOffsetUs=end-p.microsAt(tick);
    };
    std::map<int,size_t> previous;
    for (size_t i=0;i<p.notes.size();++i)
    {
        for (size_t j=0;j<8;++j)
            if (generated[i*8+j]<ranges[j][0] || generated[i*8+j]>=ranges[j][1]) throw std::runtime_error("Generated token outside bounded vocabulary");
        if (generated[i*8]-5!=p.notes[i].pitch) throw std::runtime_error("Learned model changed a score pitch");
        const auto foundPitch=previous.find(p.notes[i].pitch);
        const auto minimum=foundPitch==previous.end() ? 0 : starts[foundPitch->second]/1000+1;
        const auto allowed=intervalRange(windows[i],elapsed,minimum);
        if (generated[i*8+1]<allowed.first || generated[i*8+1]>=allowed.second) throw std::runtime_error("Generated onset exceeds the requested tempo tolerance");
        elapsed+=generated[i*8+1]-261;starts.push_back(elapsed*1000);
        const auto requested=starts.back()+(generated[i*8+3]-261)*1000;
        const auto finish=std::min(requested,endUs);result.clippedTails+=static_cast<size_t>(finish!=requested);
        if (finish-starts.back()<1000) throw std::runtime_error("Invalid learned note end");
        auto& n=output.notes[i];n.velocity=static_cast<int>(generated[i*8+2]-133);n.onsetOffsetUs=starts.back()-p.microsAt(n.tick);release(n,finish);
        if (const auto found=previous.find(n.pitch);found!=previous.end() && finishes[found->second]>starts.back())
        {
            if (starts.back()-starts[found->second]<1000) throw std::runtime_error("Unresolvable same-pitch learned articulation");
            finishes[found->second]=starts.back();release(output.notes[found->second],starts.back());++result.resolvedReleases;
        }
        previous[n.pitch]=i;finishes.push_back(finish);
    }
    std::map<int64_t,int> points;
    for (size_t i=0;i<starts.size();++i)
    {
        const auto following=i+1<starts.size() ? starts[i+1] : endUs;
        for (size_t j=0;j<4;++j) points[starts[i]+roundEven((following-starts[i])*static_cast<int64_t>(j),4)]=static_cast<int>(generated[i*8+4+j]-5261);
    }
    int last=0;
    for (const auto& [time,value]:points)
    {
        if (value==last) continue;
        const auto tick=std::clamp(roundEven(time*p.bpm*p.ppq,60000000),int64_t{0},endTick);
        output.pedals.push_back({tick,time-p.microsAt(tick),value});last=value;
    }
    output.validate();result.scoreTokens=std::move(originalTokens);result.performanceTokens=generated;return result;
}
PianoInterpretationResult PianoInterpretation::generate(PianistEngine& engine,Performance source,uint32_t seed,
    const std::function<void(float)>& progress,const std::function<void()>& checkBudget)
{
    source=score(std::move(source));const auto windows=timingWindows(source);const auto input=tokens(source);std::vector<int64_t> output;output.reserve(input.size());
    if (checkBudget) checkBudget();engine.encode(input);std::mt19937 random(seed);
    std::array<int64_t,128> pitchOnsets;pitchOnsets.fill(-1);
    int64_t previous=2,elapsedMs=0;double minimumMass=1,totalMass=0;size_t lowMassCount=0,minimumNote=0;
    for (size_t i=0;i<input.size();++i)
    {
        if (checkBudget) checkBudget();const auto logits=engine.decode(previous);
        if (i%8==0) previous=input[i];
        else
        {
            auto allowed=std::pair{ranges[i%8][0],ranges[i%8][1]};
            if (i%8==1) {allowed=intervalRange(windows[i/8],elapsedMs,pitchOnsets[static_cast<size_t>(source.notes[i/8].pitch)]+1);const auto mass=timingMass(logits,allowed.first,allowed.second);if (mass<minimumMass) {minimumMass=mass;minimumNote=i/8;}lowMassCount+=static_cast<size_t>(mass<0.01);totalMass+=mass;}
            previous=sample(logits,allowed.first,allowed.second,random);
        }
        output.push_back(previous);
        if (i%8==1) {elapsedMs+=previous-261;pitchOnsets[static_cast<size_t>(source.notes[i/8].pitch)]=elapsedMs;}
        if (progress && (i%32==0 || i+1==input.size())) progress(static_cast<float>(i+1)/static_cast<float>(input.size()));
    }
    auto result=decode(source,output);result.minimumTimingMass=minimumMass;result.meanTimingMass=totalMass/static_cast<double>(source.notes.size());
    result.lowTimingMassCount=lowMassCount;result.minimumTimingMassNote=minimumNote;return result;
}
}
