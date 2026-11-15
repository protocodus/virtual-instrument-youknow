// The U9 parameter-extremes matrix (Tests/ParameterMatrix.h) over the plug-in
// processor: every automatable parameter at its extremes, alone, in pairs and
// on every factory preset, at three rates and block sizes; and the U11 reach
// test over the same processor, every parameter the host or only the editor
// can change.
//
//   YouKnowParameterMatrixTests [--jobs N] [--rotation] [--shard K/N]
//                               [--case LABEL] [--count] [--list]
//   YouKnowParameterMatrixTests --reach [--jobs N] [--case PARAMETER]
//
// The shared command line (U9): --shard K/N runs the cases whose index modulo
// N is K (numbered from 0). The 145-program bank makes about 12,900 cases,
// hours of work even in rotation, so CTest runs one rotated shard of 256 and
// the main build's nightly run covers every shard. --reach takes no shard: it
// moves each of the 55 parameters once per value, 72 changes, and the
// rules below give each the context it is heard in, so no change has to
// search the bank for a preset where it sounds but Aging.

#include "PluginProcessor.h"

#include "ParameterMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace matrix = protocodus::matrix;

matrix::ReachRule rule (matrix::Reach reach, std::string reason = {},
                        std::vector<std::pair<std::size_t, float>> with = {}, std::string history = {})
{
    matrix::ReachRule result;
    result.reach = reach;
    result.reason = std::move (reason);
    result.with = std::move (with);
    result.history = std::move (history);
    return result;
}

class YouKnowDevice final : public matrix::JuceDevice
{
public:
    YouKnowDevice() : YouKnowDevice (std::make_unique<YouKnowAudioProcessor>()) {}

    // The processor's 20 ms message-thread timer (timerCallback) drains the
    // MIDI reflection queue and forwards the previous release's Key Mode and
    // Chorus ids to the switch pairs that replaced them. A host always runs
    // it; this harness has no message loop, so the device runs the timer's
    // two jobs through the processor's own test hooks every 20 ms of audio,
    // as YouKnow.PluginProcessor does. Without it a Chorus (legacy) lane
    // would never reach the chorus in Original timing, where only the
    // panel's own gestures and recalls reach the modelled firmware
    // (OriginalPerformance::parameters).
    void Prepare (double sampleRate, int maxBlockFrames) override
    {
        JuceDevice::Prepare (sampleRate, maxBlockFrames);
        timerFrames = std::max (1, static_cast<int> (std::lround (0.02 * sampleRate)));
        framesSinceTimer = 0;
    }

    void Process (float* left, float* right, int frames, const matrix::Event* events, int eventCount) override
    {
        JuceDevice::Process (left, right, frames, events, eventCount);
        framesSinceTimer += frames;
        if (framesSinceTimer < timerFrames)
            return;
        framesSinceTimer %= timerFrames;
        youKnow->flushPendingMidiEvents();
        youKnow->forwardLegacyModeParametersForTest();
    }

private:
    explicit YouKnowDevice (std::unique_ptr<YouKnowAudioProcessor> created)
        : JuceDevice (std::move (created)),
          youKnow (static_cast<YouKnowAudioProcessor*> (processor.get()))
    {
        const auto parameters = Parameters();
        const auto find = [&parameters] (const char* name)
        {
            for (std::size_t i = 0; i < parameters.size(); ++i)
                if (parameters[i].name == name)
                    return i;
            throw std::runtime_error (std::string ("no parameter named ") + name);
        };
        for (const auto& p : parameters)
            names.push_back (p.name);
        for (const auto& p : EditorOnlyParameters())
            names.push_back (p.name);
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
        chorusOne = find ("Chorus I");
        chorusTwo = find ("Chorus II");
        legacyChorus = find ("Chorus (legacy)");
    }

public:
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
        // With a chorus on the instrument is never silent: each MN3009 line
        // adds its hiss and, since 1.4.5, the thump of its clock at the fast
        // end of every sweep, with no key down (the processor reports an
        // infinite tail for it). On the loudest programs that idles above
        // the -60 dBFS floor (139: -52 dBFS), so silence there means the
        // idle output of an instrument set the same way and never played.
        // That peak is the clock's thump, not hiss: Chorus Noise from zero
        // to full moves it by a few tenths of a dB, and the release window,
        // which a thump lands in, peaks within 0.05 dB of it. The held
        // window falls between two thumps (about -75 dBFS there), so held
        // notes that dropped out read far below the thump, while chords
        // quiet by design sit a few dB above it: B31 Brass at Decay 0 and
        // A54 Funky II keep the VCF nearly shut (Freq 0 and 5, the envelope
        // at its 40 % and 31 % sustain, VCA in Gate), 5.5 dB (1.4.9) and
        // 4.7 dB (1.4.8) above it. 3 dB clears the thump and passes them;
        // the 6 dB this used before failed both.
        if (c.values[chorusOne] >= 0.5f || c.values[chorusTwo] >= 0.5f || c.values[legacyChorus] > 0.0f)
            limits.silenceFloor = std::max (limits.silenceFloor, 1.4125f * idlePeak (c));
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

    // U11. The base patch is a fresh instance: Original timing, Poly 1, one
    // saw, chorus off, LFO and bender depths at zero, the line output open.
    // A parameter that patch cannot sound is given here the settings that
    // let it; each declaration names the design that makes it so.
    matrix::ReachRule ReachFor (std::size_t index, float value) const override
    {
        using matrix::Reach;
        const std::string& name = names[index];
        const auto at = [this] (const char* other)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == other)
                    return i;
            std::fprintf (stderr, "ReachFor: no parameter named %s\n", other);
            std::abort();
        };
        const auto is = [&name] (const char* candidate) { return name == candidate; };
        // Performance Timing's choices are Direct (0) and Original (1).
        constexpr float direct = 0.0f;

        // The bender's depths act on the bend the lever makes. The probes are
        // bent and the held chord is not (B-2 scales the assigner's bend byte
        // by each axis's ADC on every pass).
        if (is ("Bender DCO") || is ("Bender VCF"))
            return rule (Reach::NewNotes, "the depth of the next bend; the held chord is not bent");
        // Modulation (CC 1) drives the lever's forward axis and BENDER LFO
        // sets how much LFO it adds (README, MIDI): each is silent without
        // the other (B-2 RAM 0x64 = 0x63 * the BENDER LFO byte).
        if (is ("Bender LFO"))
            return rule (Reach::Held, "", { { at ("Modulation"), 1.0f } });
        if (is ("Modulation"))
            return rule (Reach::Held, "", { { at ("Bender LFO"), 1.0f } });
        if (is ("Portamento"))
            return rule (Reach::NewNotes, "a glide into each new note", {},
                         "each voice glides from wherever the glides before left it");
        // Poly 1 rotates the six cards and Poly 2 reuses the lowest free one,
        // so which card a probe note lands on, and the cards differ by their
        // component spreads, follows the assignments the long play left.
        if (is ("Key Mode (legacy)") && value > 0.25f && value < 0.75f)
            return rule (Reach::Held, "", {}, "which card each note takes follows the assigner's earlier assignments");
        // The LFO controls act on DCO LFO's vibrato, zero on the base patch;
        // the shared LFO runs free in the phase its old rate left.
        if (is ("LFO Rate"))
            return rule (Reach::Held, "", { { at ("DCO LFO"), 0.5f } }, "the LFO runs free in the phase its old rate left");
        // B-2 restarts the delay only when a key goes down with none held;
        // the held chord's LFO has already faded in.
        if (is ("LFO Delay"))
            return rule (Reach::NewNotes, "the LFO's fade-in after a key goes down with none held", { { at ("DCO LFO"), 0.5f } });
        // PWM and its source shape the pulse, off on the base patch.
        if (is ("PWM") || is ("PWM Mode"))
            return rule (Reach::Held, "", { { at ("Pulse"), 1.0f } });
        // B-2's attack latch: a held note past its peak stays in decay.
        if (is ("Attack"))
            return rule (Reach::NewNotes, "the attack of the notes struck after it; the held chord is past its peak");
        // Original timing keeps each held key on the contact it was struck
        // on across a transpose edit (OriginalPerformance::keyboardNoteOn), as
        // the hardware's KEY TRANSPOSE moves the keys struck after it.
        if (is ("Transpose"))
            return rule (Reach::NewNotes, "the keys struck after it; a held key keeps the contact it was struck on");
        // Original timing plays the hardware's velocity-blind keybed ("Original
        // uses six cards and hardware velocity behavior", fidelity research):
        // the firmware replay gives every voice velocity 1. The VELOCITY
        // extension acts in Direct timing.
        if (is ("Velocity"))
            return rule (Reach::Held, "", { { at ("Performance Timing"), direct } });
        // The BBD hiss, only with a chorus line in the path, and heard where
        // the filter has taken the voice's harmonics away: with Chorus I on
        // the base patch full HISS moves the probe 0.07 dB, with the filter
        // at 0.1 as well 0.80 dB, the held chord 2.45 dB (2026-11-15).
        if (is ("Chorus Noise"))
            return rule (Reach::Held, "", { { at ("Chorus I"), 1.0f }, { at ("VCF Freq"), 0.1f } });
        // In Original timing the count only selects which of the first six
        // cards reach the bus in Unison, so its far end, 16, sounds as the
        // default 6 (README, Voices); Direct Unison stacks up to 16 voices.
        if (is ("Polyphony"))
            return rule (Reach::Held, "", { { at ("Poly 2"), 1.0f }, { at ("Performance Timing"), direct } });
        // Both contacts open is played as Poly 1 (keyModeFor), and the timer
        // relatches Poly 1; with Poly 2 latched, Poly 1 switches Unison to
        // Poly 2.
        if (is ("Poly 1"))
            return rule (Reach::Held, "", { { at ("Poly 2"), 1.0f } });
        // The probes' MIDI bends move the bender this parameter sets
        // (README: each axis keeps the last change from its lane or MIDI),
        // and each returns it to the centre, so change 2, back to the centre,
        // finds nothing to move. The shared settle check then compares the
        // run without it with R, which the excursion has left with its
        // free-running DCOs in other phases: 1.21 dB apart on probe 2, the
        // difference signal 4.3 dB over R (2026-11-15). This rule is the
        // design; the check, not the product, is what it fails.
        if (is ("Pitch Bend"))
            return rule (Reach::Held, "", {}, "each probe's MIDI bend moves the same bender and ends at its centre");
        // The editor-only machine settings.
        if (is ("HQ"))
            return rule (Reach::Exempt, "an inert anchor kept for saved sessions, which restore maps onto Quality; YouKnow.PluginProcessor");
        // A rung changes only once no voice is active and the output path
        // has been quiet for outputPathQuietSeconds (40 ms), which the
        // phrase, notes from its first sample to its last, never leaves: even
        // T plays on the default rung. YouKnow.Engine
        // (testQualityChangeWaitsForTheOutputPathToEmpty) and
        // YouKnow.PluginProcessor (testQualitySelectorDrivesTheEngine) show
        // it switching.
        if (is ("Quality"))
            return rule (Reach::Exempt, "the oversampling rung changes only in silence (applyPendingOversamplingIfIdle), "
                                        "which the phrase never leaves; YouKnow.Engine, YouKnow.PluginProcessor");
        // The VCF kernels were chosen as numerical policy: whole-file nulls of
        // -88...-110 dBc and no difference by ear (Docs/decisions.md,
        // 2026-08-23 and 2026-08-24). A self-oscillating preset can carry
        // the last bits into a different phase, which is no reach.
        if (is ("VCF Tanh") || is ("VCF Fast Early") || is ("VCF Solver"))
            return rule (Reach::Exempt, "a numerical VCF kernel, inaudible on the base patch; YouKnow.PluginProcessor's selector tests");
        // A receiver of 100 kOhm or 1 MOhm on the line output's source
        // impedance takes 0.37 or 0.04 dB off the probe (measured
        // 2026-11-15), under the 0.5 dB the test can hear; 10 and 47 kOhm
        // are tested.
        if (is ("Output Load") && value > 0.6f)
            return rule (Reach::Exempt, "a light receiver load, a fraction of a dB on the line output; YouKnow.OutputNetwork");
        // Cable capacitance rolls the output off from the top, so the filter
        // is opened to give it harmonics to act on. Even then the 480 pF and
        // 960 pF of a 3 m and 6 m GS-6 move the probe 0.10 and 0.22 dB (the
        // difference signal 36 and 30 dB down, 2026-11-15); 2000 and 5000 pF
        // are tested.
        if (is ("Output Capacitance"))
            return rule (value < 0.6f ? Reach::Exempt : Reach::Held,
                         value < 0.6f ? "a cable's roll-off above the audio band; YouKnow.OutputNetwork" : "",
                         { { at ("VCF Freq"), 1.0f } });
        // The headphone load acts on the PHONES amplifier only.
        if (is ("Headphone Load"))
            return rule (Reach::Held, "", { { at ("Output Route"), 1.0f } });
        return {};
    }

private:
    // The peak of an instrument set as `c` and never played: a quarter
    // second for the lines to fill, then one whole sweep of mode I, the
    // slower, so each line passes its thump.
    float idlePeak (const matrix::Case& c) const
    {
        const double rate = processor->getSampleRate();
        const int block = processor->getBlockSize();
        YouKnowDevice idle;
        idle.Prepare (rate, block);
        if (c.preset >= 0)
            idle.LoadPreset (c.preset);
        for (std::size_t i = 0; i < c.values.size(); ++i)
            idle.SetParameter (i, c.values[i]);
        std::vector<float> left (static_cast<std::size_t> (block)), right (left.size());
        const auto settle = std::llround (0.25 * rate);
        const auto total = settle + std::llround (rate / youknow::Chorus::derivedRateHz (true));
        float peak = 0.0f;
        for (long long done = 0; done < total; done += block)
        {
            idle.Process (left.data(), right.data(), block, nullptr, 0);
            if (done >= settle)
                for (std::size_t i = 0; i < left.size(); ++i)
                    peak = std::max ({ peak, std::fabs (left[i]), std::fabs (right[i]) });
        }
        return peak;
    }

    std::size_t outputTrim = 0, volume = 0, vcaLevel = 0, vcaMode = 0, sustain = 0, release = 0, poly1 = 0, poly2 = 0;
    std::size_t saw = 0, pulse = 0, sub = 0, noise = 0, resonance = 0, chorusOne = 0, chorusTwo = 0, legacyChorus = 0;
    YouKnowAudioProcessor* youKnow = nullptr;
    std::vector<std::string> names; // Parameters(), then EditorOnlyParameters()
    int timerFrames = 1, framesSinceTimer = 0;
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const auto make = [] { return std::make_unique<YouKnowDevice>(); };

    // YouKnow's own flag, stripped before the shared command line sees it:
    // --reach-shard K/N runs the reach test for the parameters whose index
    // (editor-only ones counted after the rest) modulo N is K, one --case at
    // a time. Only the sanitized build uses it; see CMakeLists.txt.
    unsigned long shard = 0, shards = 0;
    int jobs = 1;
    bool reach = false, other = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--reach-shard" && i + 1 < argc)
        {
            if (std::sscanf (argv[++i], "%lu/%lu", &shard, &shards) != 2 || shards == 0 || shard >= shards)
            {
                std::fprintf (stderr, "--reach-shard takes K/N with K < N\n");
                return 2;
            }
        }
        else if (arg == "--reach")
            reach = true;
        else if (arg == "--jobs" && i + 1 < argc)
            jobs = std::max (1, std::atoi (argv[++i]));
        else
            other = true;
    }
    if (shards == 0)
        return matrix::Main (argc, argv, make);
    if (! reach || other)
    {
        std::fprintf (stderr, "--reach-shard goes with --reach and at most --jobs N\n");
        return 2;
    }

    const auto probe = make();
    probe->Prepare (48000.0, 512);
    auto parameters = probe->Parameters();
    for (const auto& p : probe->EditorOnlyParameters())
        parameters.push_back (p);
    int failures = 0, tested = 0;
    for (std::size_t i = shard; i < parameters.size(); i += shards, ++tested)
        failures += matrix::RunReach (make, jobs, parameters[i].name);
    std::printf ("reach shard %lu/%lu: %d of %zu parameters, %d changes failed\n", shard, shards, tested,
                 parameters.size(), failures);
    return failures == 0 ? 0 : 1;
}
