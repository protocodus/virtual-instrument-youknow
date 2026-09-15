// Reproducible oscillator/envelope/C56 comparison through the product audio path.
// A is shipping, B changes exactly one mechanism. Identical fixed engine/card
// seeds, MIDI, settled thermal start, controls, 48 kHz, 128-frame blocks and
// 4x requested quality. raw/A.wav and raw/B.wav preserve level; A.wav/B.wav
// use whole-file stereo RMS matching, followed by ONE shared peak trim.
// No installed reset R/gate/clamp or hold Ron/leakage is inferred here. All
// nonzero candidate coordinates must be supplied explicitly. See the circuit
// headers for primary references and the distinction between engineering
// input domains and measured component limits. This score is not a capture.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr int block = 128;
constexpr std::uint32_t rate = 48000;

struct Candidate
{
    std::string mechanism;
    bool unison { false };
    bool complex { false };
    DcoResetCircuit::Calibration reset;
    EnvelopeHoldCircuit::Configuration hold;
    double couplingOhms {};
};

double number(const char* argument)
{
    std::size_t used = 0;
    const std::string input(argument);
    const double value = std::stod(input, &used);
    if (used != input.size() || !std::isfinite(value))
        throw std::runtime_error("candidate coordinates must be finite numbers");
    return value;
}

StereoBuffer render(const Candidate& candidate, bool enabled)
{
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
    p.calibration = 1; p.aging = 0; p.polyphony = 6;
    p.keyMode = candidate.unison ? KeyMode::Unison : KeyMode::Poly1;
    p.sawEnabled = true; p.pulseEnabled = true;
    p.pwmDepth = .72f; p.pwmSource = PwmSource::Manual;
    p.subLevel = 0; p.noiseLevel = 0; p.chorus = ChorusMode::Off;
    p.cutoff = .83f; p.resonance = .1f; p.envDepth = .18f;
    p.vcfLfoDepth = 0; p.dcoLfoDepth = 0; p.velocityDepth = 0;
    p.highPass = HighPassMode::One;
    p.attack = 0; p.decay = .12f; p.sustain = .3f; p.release = .1f;
    p.volume = .55f; p.vcaLevel = .7f;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    if (candidate.mechanism == "coupling")
    {
        p.sawEnabled = false;
        p.pwmDepth = 0;
        p.cutoff = .72f; p.resonance = .15f; p.envDepth = 0;
        p.sustain = 1; p.release = .05f;
        p.vcaMode = VcaMode::Gate;
    }
    if (candidate.complex)
    {
        p.lfoRate = .52f; p.lfoDelay = 0;
        p.sawEnabled = true; p.envDepth = 0;
        p.vcaMode = VcaMode::Gate; p.sustain = 1;
        if (candidate.mechanism == "coupling")
        {
            p.pwmSource = PwmSource::Lfo; p.pwmDepth = .75f;
            p.subLevel = .45f; p.cutoff = .55f; p.resonance = .3f;
        }
        else
        {
            p.pulseEnabled = false; p.pwmDepth = 0;
            p.cutoff = 1; p.resonance = 0;
        }
    }

    auto engine = std::make_unique<YouKnowEngine>();
    ProductFidelityProfile::configureBeforePrepare(*engine);
    if (!engine->configureThermalStart(true))
        throw std::runtime_error("cannot configure settled thermal start");
    engine->selectConverterTimingProfile(
        enabled && candidate.mechanism == "timing"
            ? YouKnowEngine::ConverterTimingProfile::FirmwareControlNoInterrupt
            : YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    if (enabled && candidate.mechanism == "reset"
        && !engine->configureDcoResetCircuit(candidate.reset))
        throw std::runtime_error("reset coordinates outside engineering domain");
    if (enabled && candidate.mechanism == "coupling"
        && !engine->configureModuleInputCouplingResistanceOhms(candidate.couplingOhms))
        throw std::runtime_error("coupling resistance outside engineering domain");
    if (enabled && candidate.mechanism == "hold")
    {
        std::array<EnvelopeHoldCircuit::Configuration, 6> holds;
        holds.fill(candidate.hold);
        if (!engine->configureEnvelopeHolds(holds))
            throw std::runtime_error("hold coordinates outside engineering domain");
    }
    engine->prepare(rate, block, 4);
    engine->setParameters(p);

    StereoBuffer audio;
    std::array<float, block> left {}, right {};
    const auto run = [&](double seconds, bool retain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * rate));
        while (remaining > 0)
        {
            const int count = std::min(block, remaining);
            engine->process(left.data(), right.data(), count);
            if (retain)
            {
                audio.left.insert(audio.left.end(), left.begin(), left.begin() + count);
                audio.right.insert(audio.right.end(), right.begin(), right.begin() + count);
            }
            remaining -= count;
        }
    };
    run(.25, false);
    if (candidate.complex && candidate.mechanism == "coupling")
    {
        // Genuine control/voice interactions, with continuous C56 and DCO
        // state. No gain boost or extra DC step is added to expose the pole.
        for (int note : { 36, 43, 50 })
        {
            engine->noteOn(note, 1); run(.2);
        }
        for (int step = 0; step < 16; ++step)
        {
            const float u = static_cast<float>(step) / 15.0f;
            p.cutoff = .35f + .25f * u;
            const float s = std::sin(static_cast<float>(3.141592653589793) * u);
            p.subLevel = .2f + .55f * s * s;
            engine->setParameters(p); run(.2);
        }
        for (int note : { 36, 43, 50 }) engine->noteOff(note);
        run(.4);

        p.vcaMode = VcaMode::Envelope;
        p.pwmSource = PwmSource::Lfo; p.pwmDepth = .85f; p.lfoRate = .65f;
        p.cutoff = .38f; p.resonance = .72f; p.envDepth = .35f;
        p.decay = .35f; p.sustain = .2f; p.release = .25f;
        engine->setParameters(p);
        for (int note : { 36, 43, 48, 55, 38, 45, 50, 57 })
        {
            engine->noteOn(note, 1); run(.3);
            engine->noteOff(note); run(.15);
        }

        p.pwmSource = PwmSource::Lfo; p.pwmDepth = .7f;
        p.cutoff = .5f; p.resonance = .4f;
        p.decay = .4f; p.sustain = .6f; p.release = .3f;
        engine->setParameters(p);
        for (int note : { 36, 43, 50, 57, 62, 67 })
        {
            engine->noteOn(note, 1); run(.12);
        }
        run(1.8);
        p.subLevel = 0; engine->setParameters(p); run(.55);
        p.subLevel = .7f; engine->setParameters(p); run(.55);
        for (int note : { 36, 43, 50, 57, 62, 67 }) engine->noteOff(note);
        run(1.58);
    }
    else if (candidate.complex && candidate.mechanism == "reset")
    {
        run(.1);
        for (int note : { 36, 60, 84 })
        {
            engine->noteOn(note, 1); run(.7);
            engine->noteOff(note); run(.2);
        }
        p.sawEnabled = false; p.pulseEnabled = true; p.pwmDepth = .1f;
        engine->setParameters(p);
        for (int note : { 60, 84, 96 })
        {
            engine->noteOn(note, 1); run(.7);
            engine->noteOff(note); run(.2);
        }
        p.sawEnabled = true; p.pwmSource = PwmSource::Lfo; p.pwmDepth = .7f;
        p.subLevel = .5f; p.cutoff = .65f; p.resonance = .25f;
        p.envDepth = .25f; p.vcaMode = VcaMode::Envelope;
        p.decay = .25f; p.sustain = .5f; p.release = .3f;
        engine->setParameters(p);
        for (int note : { 36, 43, 50, 60, 67, 74 })
        {
            engine->noteOn(note, 1); run(.15);
        }
        run(2.1);
        for (int note : { 36, 43, 50, 60, 67, 74 }) engine->noteOff(note);
        run(.5);
        for (int note : { 48, 55, 62, 72, 79, 86 })
        {
            engine->noteOn(note, 1); run(.1);
        }
        run(1.4);
        for (int note : { 48, 55, 62, 72, 79, 86 }) engine->noteOff(note);
        run(2.0);
    }
    else if (candidate.mechanism == "coupling")
    {
        // Hold one voice while real panel changes move WAVE's mean. Follow
        // with fixed-control bass and resonant plucks: note-on itself never
        // reconnects the oscillator or clears C56. All letters share this score.
        engine->noteOn(36, 1); run(.6);
        p.pwmDepth = .8f; engine->setParameters(p); run(.6);
        p.pwmDepth = 0; engine->setParameters(p); run(.6);
        p.subLevel = .65f; engine->setParameters(p); run(.6);
        p.subLevel = 0; engine->setParameters(p); run(.5);
        engine->noteOff(36); run(.5);

        p.sawEnabled = true; p.pwmDepth = .7f; p.subLevel = .65f;
        p.vcaMode = VcaMode::Envelope;
        p.cutoff = .48f; p.resonance = .15f; p.envDepth = .32f;
        p.attack = 0; p.decay = .18f; p.sustain = 0; p.release = .08f;
        engine->setParameters(p);
        for (int note : { 36, 36, 31 })
        {
            engine->noteOn(note, 1); run(.35);
            engine->noteOff(note); run(.4);
        }
        run(.55);
        p.cutoff = .38f; p.resonance = .75f; p.envDepth = .45f;
        p.decay = .25f; engine->setParameters(p);
        for (int note : { 36, 48, 43 })
        {
            engine->noteOn(note, 1); run(.3);
            engine->noteOff(note); run(.5);
        }
        run(.8);
    }
    else
    {
        run(.05);
        // Low/high carrier rates expose reset charge and pulse crossings; short
        // re-presses and overlapping voices expose envelope stores and acquisition.
        for (int note : { 45, 69, 93 })
        {
            engine->noteOn(note, 1);
            run(.45);
            engine->noteOff(note);
            run(.025);
            engine->noteOn(note, 1);
            run(.15);
            engine->noteOff(note);
            run(.25);
        }
        for (int note : { 48, 55, 60, 64, 67, 72 })
        {
            engine->noteOn(note, 1);
            run(.075);
        }
        run(.65);
        for (int note : { 48, 55, 60, 64, 67, 72 })
            engine->noteOff(note);
        run(.9);
    }
    std::string error;
    if (!validate(audio, error) || measure(audio).rms < 1e-8)
        throw std::runtime_error("invalid comparison render: " + error);
    return audio;
}
}

int main(int argc, char** argv)
{
    try
    {
        bool unison = false, complex = false;
        while (argc > 1)
        {
            const std::string flag(argv[argc - 1]);
            if (flag == "--unison" && !unison) unison = true;
            else if (flag == "--complex" && !complex) complex = true;
            else break;
            --argc;
        }
        if (argc < 3)
            throw std::runtime_error(
                "usage: YouKnowRenderOscillatorEnvelopeCircuits "
                "reset OUT OHMS GATE_US CLAMP_VOLTS | "
                "hold OUT OHMS [BIAS_NA LEAKAGE_NA CHARGE_PC] | "
                "coupling OUT TOTAL_OHMS | timing OUT; "
                "append --unison for the six-voice stack; "
                "--complex selects richer reset/coupling material");
        Candidate candidate;
        candidate.mechanism = argv[1];
        candidate.unison = unison;
        candidate.complex = complex;
        if (complex && candidate.mechanism != "reset" && candidate.mechanism != "coupling")
            throw std::runtime_error("--complex supports reset and coupling only");
        if (candidate.mechanism == "coupling" && argc == 4)
            candidate.couplingOhms = number(argv[3]);
        else if (candidate.mechanism == "reset" && argc == 6)
        {
            // JUNO-6/60 CPU p.9 prints R35 = 2.2 OHMS (no K), R34 = 10k,
            // R33 = 1k, C6 = 270pF and a TL082 Miller integrator. A prior
            // audition misread R35 as 2.2k. Neither that erroneous RC nor
            // ln(100)*R35*C54 establishes gate duration. At the real small
            // resistance, transistor drive and amplifier slew/recovery matter;
            // this resistor-only reset model does not include those limits.
            // https://www.synfo.nl/servicemanuals/Roland/JUNO-6_SERVICE_NOTES.pdf#page=9
            candidate.reset.dischargeOhms = number(argv[3]);
            candidate.reset.gateSeconds = number(argv[4]) * 1e-6;
            candidate.reset.clampVolts = number(argv[5]);
        }
        else if (candidate.mechanism == "hold" && (argc == 4 || argc == 7))
        {
            candidate.hold.onResistanceOhms = number(argv[3]);
            if (argc == 7)
            {
                candidate.hold.inputBiasAmps = number(argv[4]) * 1e-9;
                candidate.hold.offLeakageAmps = number(argv[5]) * 1e-9;
                candidate.hold.turnOffChargeCoulombs = number(argv[6]) * 1e-12;
            }
        }
        else if (candidate.mechanism != "timing" || argc != 3)
            throw std::runtime_error("unknown mechanism or incorrect argument count");

        const std::filesystem::path directory(argv[2]);
        if (std::filesystem::exists(directory) && !std::filesystem::is_empty(directory))
            throw std::runtime_error("output directory must be new or empty");
        // Render B first so invalid physical coordinates fail before any files
        // or expensive baseline render. The comparison identity stays A/B.
        const auto b = render(candidate, true);
        const auto a = render(candidate, false);
        const auto aLevel = measure(a), bLevel = measure(b);
        const double rmsMatch = aLevel.rms / bLevel.rms;
        const double sharedTrim = listeningTargetPeak
            / std::max(aLevel.peak, bLevel.peak * rmsMatch);
        StereoBuffer delta;
        std::string error;
        if (!difference(a, b, delta, error)) throw std::runtime_error(error);
        for (const auto& item : std::array {
                 std::pair { "raw/A.wav", a }, std::pair { "raw/B.wav", b },
                 std::pair { "A.wav", applyGain(a, sharedTrim) },
                 std::pair { "B.wav", applyGain(b, sharedTrim * rmsMatch) } })
            if (!writeFloatWav(directory / item.first, item.second, error, rate))
                throw std::runtime_error(error);

        std::ofstream key(directory / "key.md");
        key << std::setprecision(12)
            << "A: shipping engine. B: " << candidate.mechanism << " candidate.\n\n"
            << "Key mode: " << (candidate.unison ? "six-voice Unison" : "Poly 1") << ".\n\n"
            << "DSP source SHA-256: `" << YOUKNOW_DSP_SOURCE_SHA256 << "`.\n\n"
            << "Identical product audio path, fixed engine/card seeds, MIDI, "
               "48 kHz, 128-frame blocks, requested 4x quality, PolyZoned/Cubic/"
               "Rk4Single, settled thermal start; 0.25 s discarded preroll. "
               "Only the named mechanism differs. These are engineering "
               "comparisons, not calibrated Juno-106 measurements.\n\n";
        if (candidate.mechanism == "reset")
            key << "Reset: " << candidate.reset.dischargeOhms << " ohms, "
                << candidate.reset.gateSeconds << " s gate, "
                << candidate.reset.clampVolts << " V clamp.\n\n";
        if (candidate.mechanism == "coupling")
            key << "C56 remains 10 uF; B total effective source/load resistance: "
                << candidate.couplingOhms << " ohms. A uses "
                << ProductFidelityProfile::moduleInputCouplingResistanceOhms
                << " ohms from the current product profile. "
                   "This changes the existing coupling pole only; WAVE gain, "
                   "mixer law, filter and continuous capacitor state remain intact. "
                   "An effective resistance is a reduced circuit assumption, "
                   "not a full frequency-dependent input-network solve.\n\n";
        if (candidate.mechanism == "hold")
            key << "Each of six independent 10 nF holds: "
                << candidate.hold.onResistanceOhms << " ohms, "
                << candidate.hold.inputBiasAmps << " A bias, "
                << candidate.hold.offLeakageAmps << " A off leakage, "
                << candidate.hold.turnOffChargeCoulombs << " C turn-off charge.\n\n";
        if (candidate.mechanism == "timing")
            key << "B uses full B-2 no-interrupt execution; pass-start host/ADC "
                   "snapshots and nominal instruction timing. Physical ISR, "
                   "serial transport and pin-edge delays are excluded.\n\n";
        key << "## Score\n\n";
        if (candidate.complex && candidate.mechanism == "coupling")
            key << "13 s. 0-3.8 s: MIDI 36/43/50 enter 0.2 s apart, LFO PWM "
                   "depth 0.75/rate panel 0.52; cutoff ramps 0.35 to 0.60 and "
                   "SUB follows 0.2 + 0.55*sin(pi*u)^2 in 16 snapshots at "
                   "0.6 + 0.2*j s (u=j/15). Release at 3.8 s. "
                   "4.2-7.8 s: MIDI 36/43/48/55/38/45/50/57 at 0.45 s "
                   "spacing, held 0.3 s, LFO PWM depth 0.85/rate 0.65, "
                   "cutoff 0.38/resonance 0.72/filter ENV 0.35, decay 0.35/"
                   "sustain 0.2/release 0.25. 7.8-8.52 s: MIDI "
                   "36/43/50/57/62/67 enter 0.12 s apart; PWM depth 0.7, "
                   "cutoff 0.5/resonance 0.4, decay 0.4/sustain 0.6/release "
                   "0.3. SUB switches to 0 at 10.32 s and 0.7 at 10.87 s; "
                   "all notes off at 11.42 s, tail to 13 s. Chorus stays off. "
                   "All normalized controls, exact implementation pinned by "
                   "source hash; no capacitor or oscillator resets between sections.\n\n";
        else if (candidate.complex)
            key << "13 s. 0.1/1.0/1.9 s: saw-only MIDI 36/60/84, each "
                   "held 0.7 s, 0.2 s gaps, open filter/zero RES/ENV and gate VCA. "
                   "2.8/3.7/4.6 s: pulse-only MIDI 60/84/96, manual PWM 0.1. "
                   "5.5 s: mixed saw/pulse/SUB 0.5; LFO PWM 0.7/rate 0.52, "
                   "cutoff 0.65/RES 0.25/ENV 0.25, envelope VCA, decay 0.25/"
                   "sustain 0.5/release 0.3. MIDI 36/43/50/60/67/74 enter "
                   "0.15 s apart, all off 8.5 s. MIDI 48/55/62/72/79/86 enter "
                   "0.1 s apart from 9 s, all off at 11 s, tail to 13 s. "
                   "Chorus stays off; continuous oscillator/capacitor state.\n\n";
        else if (candidate.mechanism == "coupling")
            key << "9.4 s: 0-2.9 s held MIDI 36 with manual PWM changes at "
                   "0.6/1.2 s and SUB changes at 1.8/2.4 s; bass reattacks at "
                   "3.4/4.15/4.9 s; resonant plucks at 6.2/7.0/7.8 s, then tail.\n\n";
        else
            key << "4.675 s: low/high carrier re-presses, then a six-note "
                   "overlapping chord and release; original short score.\n\n";
        key << "## Level matching\n\n"
            << "Whole-file stereo RMS matching; a common final trim keeps "
               "the louder peak at -6 dBFS. Raw files preserve amplitude.\n\n"
            << "| | Raw peak dBFS | Raw RMS dBFS | Listening trim dB |\n"
               "|---|---:|---:|---:|\n"
            << "| A | " << decibels(aLevel.peak) << " | " << decibels(aLevel.rms)
            << " | " << decibels(sharedTrim) << " |\n"
            << "| B | " << decibels(bLevel.peak) << " | " << decibels(bLevel.rms)
            << " | " << decibels(sharedTrim * rmsMatch) << " |\n\n"
            << "Unmatched B-A RMS relative to A: "
            << decibels(measure(delta).rms / aLevel.rms) << " dBr.\n";
        key.close();
        if (!key) throw std::runtime_error("cannot write comparison key");
        std::cout << "Wrote A.wav, B.wav, raw pair and key.md to " << directory << '\n';
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
