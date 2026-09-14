// Reproducible O3/E2/E3 circuit comparison through the product audio path.
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
    DcoResetCircuit::Calibration reset;
    EnvelopeHoldCircuit::Configuration hold;
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
        const bool unison = argc > 1 && std::string(argv[argc - 1]) == "--unison";
        if (unison) --argc;
        if (argc < 3)
            throw std::runtime_error(
                "usage: YouKnowRenderOscillatorEnvelopeCircuits "
                "reset OUT OHMS GATE_US CLAMP_VOLTS | "
                "hold OUT OHMS [BIAS_NA LEAKAGE_NA CHARGE_PC] | timing OUT; "
                "append --unison for the six-voice stack");
        Candidate candidate;
        candidate.mechanism = argv[1];
        candidate.unison = unison;
        if (candidate.mechanism == "reset" && argc == 6)
        {
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
        key << "Whole-file stereo RMS matching; a common final trim keeps "
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
