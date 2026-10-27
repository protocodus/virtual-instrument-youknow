// The U9 parameter-extremes matrix (Tests/ParameterMatrix.h) over the plug-in
// processor: every automatable parameter at its extremes, alone, in pairs and
// on every factory preset, at three rates and block sizes.
//
//   YouKnowParameterMatrixTests [--jobs N] [--rotation] [--shard K/N]
//                               [--case LABEL] [--count] [--list]
//
// The shared command line (U9): --shard K/N runs the cases whose index modulo
// N is K (numbered from 0). The 145-program bank makes about 12,900 cases,
// hours of work even in rotation, so CTest runs one rotated shard of 256 and
// the main build's nightly run covers every shard.

#include "PluginProcessor.h"

#include "ParameterMatrix.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
namespace matrix = protocodus::matrix;

class YouKnowDevice final : public matrix::JuceDevice
{
public:
    YouKnowDevice() : JuceDevice (std::make_unique<YouKnowAudioProcessor>())
    {
        const auto parameters = Parameters();
        const auto find = [&parameters] (const char* name)
        {
            for (std::size_t i = 0; i < parameters.size(); ++i)
                if (parameters[i].name == name)
                    return i;
            throw std::runtime_error (std::string ("no parameter named ") + name);
        };
        outputTrim = find ("Output Trim");
        volume = find ("Volume");
        vcaLevel = find ("VCA Level");
        vcaMode = find ("VCA Mode");
        sustain = find ("Sustain");
        release = find ("Release");
        poly1 = find ("Poly 1");
        poly2 = find ("Poly 2");
        saw = find ("Saw");
        pulse = find ("Pulse");
        sub = find ("Sub");
        noise = find ("Noise");
        resonance = find ("VCF Res");
    }

    matrix::Limits LimitsFor (const matrix::Case& c) const override
    {
        matrix::Limits limits;
        // Digital full scale is the output summer's clip asymptote; the
        // engine's output-level policy then adds 2.5 dB after the clip, so a
        // patch driven to that asymptote (every control at maximum, in
        // unison) peaks up to +2.5 dBFS and no further.
        limits.ceiling = youknow::YouKnowEngine::outputLevelPolicyGain;
        // Output Trim is a plain -48..+48 dB gain after the circuit, so the
        // ceiling, silence floor and DC limit move with it; automation ramps
        // towards base, so the louder of the two decides.
        const auto decibels = [] (float normalised) { return -48.0f + 96.0f * normalised; };
        const float gain = std::pow (10.0f, std::max (decibels (c.values[outputTrim]),
                                                      decibels (c.base[outputTrim])) / 20.0f);
        if (gain > 1.0f)
        {
            limits.ceiling *= gain;
            limits.silenceFloor *= gain;
            limits.dcLimit *= gain;
        }
        // In Env mode the voice amplifier follows the envelope's release,
        // up to tens of seconds at full travel.
        const auto envMode = [] (float normalised) { return normalised < 0.5f; };
        if (envMode (c.values[vcaMode]) || envMode (c.base[vcaMode]))
        {
            const float seconds = youknow::YouKnowEngine::envelopeReleaseSeconds (
                std::max (c.values[release], c.base[release]));
            limits.releaseSeconds = std::max (limits.releaseSeconds, static_cast<double> (seconds) + 0.25);
        }
        // Silent by design: Volume or VCA Level at zero, or Output Trim at
        // its -48 dB end, which puts any held chord quieter than -12 dBFS
        // under the floor (A54 Funky II's sits near -48 dBFS before its
        // trim). So are plucks and percussion: by the held window the
        // envelope has decayed to a Sustain of zero or a little above, and
        // has closed the filter it also drives (in Gate mode too), as YB2
        // Pulse Pluck at 12 % sits near -68 dBFS before its trim. A quarter
        // of travel separates those presets from the sustained ones. And so
        // is a voice with no source: Saw and Pulse off, Sub and Noise at
        // zero, and no resonance for the filter to ring with, which is how
        // B75 High Bells sounds at VCF Res 0.
        const auto noSource = [this] (const std::vector<float>& row)
        {
            return row[saw] < 0.5f && row[pulse] < 0.5f && row[sub] <= 0.0f
                && row[noise] <= 0.0f && row[resonance] <= 0.0f;
        };
        if (std::min (c.values[volume], c.base[volume]) <= 0.0f
            || std::min (c.values[vcaLevel], c.base[vcaLevel]) <= 0.0f
            || std::min (c.values[outputTrim], c.base[outputTrim]) <= 0.0f
            || std::min (c.values[sustain], c.base[sustain]) < 0.25f
            || noSource (c.values) || noSource (c.base))
            limits.soundsWhileHeld = false;
        return limits;
    }

    // Both POLY contacts open is not a hardware state: the engine plays it
    // as Poly 1 (keyModeFor) and a restored session stores Poly 1. Report the
    // mode the engine runs, so the save-and-restore check compares states
    // rather than that one documented canonicalisation.
    float GetParameter (std::size_t index) const override
    {
        if (index == poly1 && JuceDevice::GetParameter (poly1) < 0.5f && JuceDevice::GetParameter (poly2) < 0.5f)
            return 1.0f;
        return JuceDevice::GetParameter (index);
    }

private:
    std::size_t outputTrim = 0, volume = 0, vcaLevel = 0, vcaMode = 0, sustain = 0, release = 0, poly1 = 0, poly2 = 0;
    std::size_t saw = 0, pulse = 0, sub = 0, noise = 0, resonance = 0;
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    return matrix::Main (argc, argv, [] { return std::make_unique<YouKnowDevice>(); });
}
