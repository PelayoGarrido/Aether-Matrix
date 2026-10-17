#pragma once
#include "Performance.h"
#include "PianistEngine.h"
#include <functional>

namespace aether
{
struct PianoInterpretationResult
{
    Performance performance;
    std::vector<int64_t> scoreTokens,performanceTokens;
    size_t clippedTails=0,resolvedReleases=0,lowTimingMassCount=0,minimumTimingMassNote=0;
    double minimumTimingMass=1,meanTimingMass=1;
};
class PianoInterpretation
{
public:
    static Performance score(Performance);
    static std::vector<int64_t> tokens(const Performance& sortedScore);
    static std::pair<int,int> timingRange(const Performance&,size_t note,int64_t elapsedMs,int64_t minimumOnsetMs=0);
    static PianoInterpretationResult decode(const Performance&,const std::vector<int64_t>& generated);
    static PianoInterpretationResult generate(PianistEngine&,Performance,uint32_t seed,
        const std::function<void(float)>& progress={},const std::function<void()>& checkBudget={});
};
}
