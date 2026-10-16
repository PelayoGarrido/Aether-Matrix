#pragma once
#include <algorithm>
#include <cmath>

namespace aether::audio
{
// Polynomial band-limited step correction; phase is normalised to [0, 1).
// This reduces discontinuity aliasing, but is not an oversampled nonlinear stage.
inline float polyBlep(double phase, double increment) noexcept
{
    if (!(increment > 0.0 && increment < 0.5)) return 0.0f;
    if (phase < increment)
    {
        const double t = phase / increment;
        return static_cast<float>(2.0 * t - t * t - 1.0);
    }
    if (phase > 1.0 - increment)
    {
        const double t = (phase - 1.0) / increment;
        return static_cast<float>(t * t + 2.0 * t + 1.0);
    }
    return 0.0f;
}
inline float saw(double phase, double increment) noexcept
{
    return static_cast<float>(2.0 * phase - 1.0) - polyBlep(phase, increment);
}
inline float pulse(double phase, double increment, double width) noexcept
{
    width = std::clamp(width, increment, 1.0 - increment);
    double falling = phase - width;
    if (falling < 0.0) falling += 1.0;
    return (phase < width ? 0.5f : -0.5f)
        + 0.5f * (polyBlep(phase, increment) - polyBlep(falling, increment));
}
inline float partialGain(double frequency, double sampleRate) noexcept
{
    // Fade the upper partials before Nyquist rather than folding them into the band.
    return static_cast<float>(std::clamp((sampleRate * 0.475 - frequency) / (sampleRate * 0.025), 0.0, 1.0));
}
struct Decay
{
    float lead = 1.0f, sustained = 1.0f, bass = 1.0f, chord = 1.0f;
    static Decay forRate(double sampleRate, float contour) noexcept
    {
        const double ratio = 48000.0 / sampleRate;
        const auto coefficient = [ratio](double at48k) { return static_cast<float>(std::pow(at48k, ratio)); };
        contour = std::clamp(contour, 0.05f, 1.0f);
        return { coefficient(1.0 - ((1.0 - contour) * 0.0010 + 0.00004)),
                 coefficient(0.99985), coefficient(0.9996 + 0.0003 * contour),
                 coefficient(0.9997 + 0.0002 * contour) };
    }
};
}
