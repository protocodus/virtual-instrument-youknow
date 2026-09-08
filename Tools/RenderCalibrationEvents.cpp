// Render timestamped MIDI exported by AnalyzeHardwareCalibration.py. The
// hardware comparison uses the source file's actual SysEx and tempo, avoiding
// hand-transcribed patches and host-dependent MIDI-file playback speed.
#include "DSP/YouKnowSysEx.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <chrono>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static void primeRotaryAssignment(YouKnowEngine& engine, int slot, int midiNote)
    {
        // Service Notes p.19 uses POLY I+II as rotary assignment in TEST
        // mode. The public fullsweep capture explicitly used that mode.
        // Prime only assigner note memory so the ordinary noteOn enters the
        // requested physical slot; every oscillator/filter/control state and
        // the live product allocator implementation remain untouched.
        for (int card = 0; card < YouKnowEngine::hardwareVoices; ++card)
        {
            auto& voice = engine.voices_[static_cast<std::size_t>(card)];
            if (voice.keyDown)
                throw std::runtime_error("rotary fixture requires non-overlapping keys");
            voice.hasAllocatorHistory = true;
            voice.lastRootMidi = card == slot ? midiNote : -1;
        }
    }
    static int heldSlot(const YouKnowEngine& engine)
    {
        for (int slot = 0; slot < YouKnowEngine::hardwareVoices; ++slot)
            if (engine.voices_[static_cast<std::size_t>(slot)].keyDown)
                return slot;
        return -1;
    }
};
}

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;

bool shippingMode(const std::string& name)
{
    if (name == "shipping")
        return true;
    if (name == "exact")
        return false;
    throw std::runtime_error("kernel must be exact or shipping");
}

bool effectiveChorusProfile(const std::string& name)
{
    if (name == "nominal")
        return false;
    if (name == "a11-effective")
        return true;
    throw std::runtime_error("chorus profile must be nominal or a11-effective");
}

struct RenderOptions
{
    float character { 1.0f };
    bool shipping { false };
    float noiseScale { 1.0f };
    bool a11EffectiveChorus { false };
    bool fixedServiceTrim { true };
    bool referenceVcf { false };
    std::uint32_t sampleRate { comparisonSampleRate };
    int oversampleFactor { 4 };
    bool rotary { false };
};

RenderOptions readOptions(const std::vector<std::string>& arguments)
{
    if (arguments.size() > 9)
        throw std::runtime_error("too many render options");
    const auto finiteRange = [](const std::string& text, float maximum, const char* label) {
        std::size_t used;
        const float value = std::stof(text, &used);
        if (used != text.size() || !std::isfinite(value) || value < 0.0f || value > maximum)
            throw std::runtime_error(std::string(label) + " must be finite and in 0.."
                                     + std::to_string(static_cast<int>(maximum)));
        return value;
    };
    RenderOptions result;
    if (!arguments.empty())
        result.character = finiteRange(arguments[0], 2.0f, "character");
    if (arguments.size() >= 2)
        result.shipping = shippingMode(arguments[1]);
    if (arguments.size() >= 3)
        result.noiseScale = finiteRange(arguments[2], 4.0f, "noise scale");
    if (arguments.size() >= 4)
        result.a11EffectiveChorus = effectiveChorusProfile(arguments[3]);
    const auto choice = [](const std::string& value, const char* off, const char* on) {
        if (value == off) return false;
        if (value == on) return true;
        throw std::runtime_error(std::string("option must be ") + off + " or " + on);
    };
    if (arguments.size() >= 5)
        result.fixedServiceTrim = choice(arguments[4], "dynamic", "fixed-service");
    if (arguments.size() >= 6)
        result.referenceVcf = choice(arguments[5], "nominal", "serviced439522");
    if (arguments.size() >= 7)
    {
        std::size_t used;
        const auto rate = std::stoul(arguments[6], &used);
        if (used != arguments[6].size() || rate < 8000u || rate > 768000u)
            throw std::runtime_error("sample rate must be an integer in 8000..768000");
        result.sampleRate = static_cast<std::uint32_t>(rate);
    }
    if (arguments.size() >= 8)
    {
        const auto& factor = arguments[7];
        if (factor != "1" && factor != "2" && factor != "4")
            throw std::runtime_error("oversample factor must be 1, 2 or 4");
        result.oversampleFactor = std::stoi(factor);
    }
    if (arguments.size() >= 9)
        result.rotary = choice(arguments[8], "normal", "rotary6");
    return result;
}

EngineParameters parametersFor(const sysex::Patch& patch, float character,
                               bool shipping, float noiseScale = 1.0f,
                               bool a11EffectiveChorus = false,
                               bool fixedServiceTrim = true,
                               bool referenceVcf = false)
{
    EngineParameters p;
    p.lfoRate = patch.lfoRate; p.lfoDelay = patch.lfoDelay;
    p.dcoLfoDepth = patch.dcoLfo; p.pwmDepth = patch.pwm;
    p.pwmSource = patch.pwmSource; p.range = patch.range;
    p.sawEnabled = patch.saw; p.pulseEnabled = patch.pulse;
    p.subLevel = patch.sub; p.noiseLevel = patch.noise;
    p.highPass = patch.highPass; p.cutoff = patch.cutoff;
    p.resonance = patch.resonance; p.envPolarity = patch.envPolarity;
    p.envDepth = patch.vcfEnv; p.vcfLfoDepth = patch.vcfLfo;
    p.keyFollow = patch.keyFollow; p.vcaMode = patch.vcaMode;
    p.vcaLevel = patch.vcaLevel; p.attack = patch.attack;
    p.decay = patch.decay; p.sustain = patch.sustain;
    p.release = patch.release; p.chorus = patch.chorus;
    p.volume = 1.0f; p.polyphony = 6; p.calibration = character;
    // A measured source-level candidate is independent of the recording's
    // output gain. Keep it ahead of the actual filter/VCA/nonlinearities,
    // exactly where the shared noise rail enters the shipping engine.
    p.mainNoiseLevelScale = noiseScale;
    p.useA11EffectiveChorusTimingProfile = a11EffectiveChorus;
    p.useFixedVcfServiceFrequencyTrim = fixedServiceTrim;
    p.useServiced439522VcfCalibration = referenceVcf;
    // Match fresh plug-in instances, including the inactive-card/chorus skips.
    // Keep Exact/Merson as the default for historical hardware comparisons.
    // PluginProcessor's public defaults are Poly (2), Cubic (1), RK4 x1 (2).
    if (shipping)
    {
        p.vcfTanhMode = VcfTanhMode::PolyZoned;
        p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
    }
    return p;
}

bool patchParametersForEvent(const std::vector<std::uint8_t>& bytes,
                             sysex::Patch& patch, bool& havePatch,
                             const RenderOptions& options, EngineParameters& result)
{
    int channel, parameter, value;
    if (sysex::readPatchMessage(bytes.data(), bytes.size(), patch, channel))
        havePatch = true;
    else if (!(havePatch && sysex::readParameterMessage(
                   bytes.data(), bytes.size(), parameter, value, channel)
               && sysex::applyParameter(patch, parameter, value)))
        return false;
    // Both a new full patch and a single-control update preserve the render's
    // explicitly selected comparison coordinates.
    result = parametersFor(patch, options.character, options.shipping,
                           options.noiseScale, options.a11EffectiveChorus,
                           options.fixedServiceTrim, options.referenceVcf);
    return true;
}

struct Event
{
    std::size_t frame;
    std::vector<std::uint8_t> bytes;
};

std::vector<Event> readEvents(std::istream& input,
                              std::uint32_t sampleRate = comparisonSampleRate)
{
    std::vector<Event> events;
    std::string line;
    double previous = 0.0;
    while (std::getline(input, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream row(line);
        double seconds;
        std::string hex, extra;
        if (!(row >> seconds >> hex) || (row >> extra)
            || !std::isfinite(seconds) || seconds < previous || seconds > 3600.0
            || hex.size() < 6 || hex.size() > 48 || hex.size() % 2 != 0)
            throw std::runtime_error("invalid or unordered event row: " + line);
        Event event { static_cast<std::size_t>(std::llround(seconds * sampleRate)), {} };
        for (std::size_t i = 0; i < hex.size(); i += 2)
        {
            const auto pair = hex.substr(i, 2);
            if (pair.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
                throw std::runtime_error("invalid hexadecimal MIDI byte");
            event.bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
        }
        events.push_back(std::move(event));
        previous = seconds;
    }
    if (!input.eof() || events.empty())
        throw std::runtime_error("empty or unreadable event file");
    return events;
}

void selfTest()
{
    const auto defaults = readOptions({});
    const auto lower = readOptions({ "0", "exact", "0", "nominal" });
    const auto upper = readOptions({ "2", "shipping", "4", "a11-effective" });
    const auto selected = readOptions({ "0.75", "shipping", "3.44", "a11-effective" });
    if (defaults.character != 1.0f || defaults.shipping || defaults.noiseScale != 1.0f
        || defaults.a11EffectiveChorus || lower.character != 0.0f || lower.shipping
        || lower.noiseScale != 0.0f || lower.a11EffectiveChorus
        || upper.character != 2.0f || !upper.shipping || upper.noiseScale != 4.0f
        || !upper.a11EffectiveChorus || selected.noiseScale != 3.44f)
        throw std::runtime_error("render option defaults, endpoints or combined candidates changed");
    const auto cardProfile = readOptions({ "0", "shipping", "1", "nominal",
        "fixed-service", "serviced439522", "192000", "1", "rotary6" });
    const auto legacy = readOptions({ "0", "shipping", "1", "nominal", "dynamic" });
    if (!defaults.fixedServiceTrim || defaults.referenceVcf || defaults.sampleRate != 48000u
        || defaults.oversampleFactor != 4 || defaults.rotary || legacy.fixedServiceTrim
        || !cardProfile.fixedServiceTrim || !cardProfile.referenceVcf
        || cardProfile.sampleRate != 192000u || cardProfile.oversampleFactor != 1
        || !cardProfile.rotary)
        throw std::runtime_error("voice-card profile/rate/rotary options changed");
    for (const auto& invalid : std::vector<std::vector<std::string>> {
             { "0", "shipping", "1", "nominal", "fixed-service", "wrong" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "7999" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "192000.001" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "768001" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "192000", "3" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "192000", "1", "wrong" },
             { "0", "shipping", "1", "nominal", "dynamic", "nominal", "192000", "1", "normal", "extra" } })
    {
        bool rejected = false;
        try { (void) readOptions(invalid); }
        catch (const std::exception&) { rejected = true; }
        if (!rejected)
            throw std::runtime_error("invalid voice-card render option was accepted");
    }
    for (const auto* invalid : { "", "nan", "inf", "-inf", "-0.01", "1junk", "1e1000" })
        for (const bool noise : { false, true })
        {
            bool rejected = false;
            try
            {
                (void) readOptions(noise
                    ? std::vector<std::string> { "1", "shipping", invalid, "a11-effective" }
                    : std::vector<std::string> { invalid });
            }
            catch (const std::exception&) { rejected = true; }
            if (!rejected)
                throw std::runtime_error("invalid numeric render option was accepted");
        }
    for (const auto& invalid : std::vector<std::vector<std::string>> {
             { "2.01" }, { "1", "shipping", "4.01" },
             { "1", "shipping", "3.44", "a11" },
             { "1", "shipping", "3.44", "nominal", "extra" } })
    {
        bool rejected = false;
        try { (void) readOptions(invalid); }
        catch (const std::exception&) { rejected = true; }
        if (!rejected)
            throw std::runtime_error("invalid render option combination was accepted");
    }
    const auto exact = parametersFor(sysex::Patch {}, 0.0f, shippingMode("exact"));
    const auto shipping = parametersFor(sysex::Patch {}, 1.0f, shippingMode("shipping"));
    if (exact.vcfTanhMode != VcfTanhMode::Exact
        || exact.vcfSolverMode != VcfSolverMode::MersonHalfSteps
        || shipping.vcfTanhMode != VcfTanhMode::PolyZoned
        || shipping.vcfFastEarlyMode != VcfFastEarlyMode::Cubic
        || shipping.vcfSolverMode != VcfSolverMode::Rk4Single
        || exact.calibration != 0.0f || shipping.calibration != 1.0f)
        throw std::runtime_error("calibration renderer kernel selection changed");
    bool invalidKernelRejected = false;
    try { (void) shippingMode("shippng"); }
    catch (const std::runtime_error&) { invalidKernelRejected = true; }
    if (!invalidKernelRejected)
        throw std::runtime_error("unknown kernel was accepted");
    const auto profile = parametersFor(sysex::Patch {}, 1.0f, true, 1.0f,
                                       effectiveChorusProfile("a11-effective"));
    if (!profile.useA11EffectiveChorusTimingProfile
        || shipping.useA11EffectiveChorusTimingProfile
        || effectiveChorusProfile("nominal"))
        throw std::runtime_error("chorus profile selection changed the nominal default");
    bool invalidProfileRejected = false;
    try { (void) effectiveChorusProfile("a11"); }
    catch (const std::runtime_error&) { invalidProfileRejected = true; }
    if (!invalidProfileRejected)
        throw std::runtime_error("unknown chorus profile was accepted");

    sysex::Patch source, decoded;
    source.chorus = ChorusMode::One;
    source.cutoff = 0.2f;
    std::vector<std::uint8_t> fullPatch(sysex::patchMessageBytes);
    if (sysex::writePatchMessage(source, 0, fullPatch.data(), fullPatch.size()) != fullPatch.size())
        throw std::runtime_error("patch-update fixture did not encode");
    bool havePatch = false;
    EngineParameters changed;
    const auto checkSelection = [&] {
        if (changed.mainNoiseLevelScale != selected.noiseScale
            || changed.useA11EffectiveChorusTimingProfile != selected.a11EffectiveChorus
            || changed.calibration != selected.character
            || changed.vcfTanhMode != VcfTanhMode::PolyZoned
            || changed.cutoff != decoded.cutoff || changed.chorus != decoded.chorus)
            throw std::runtime_error("a decoded patch update lost the selected render options");
    };
    if (!patchParametersForEvent(fullPatch, decoded, havePatch, selected, changed) || !havePatch)
        throw std::runtime_error("full patch update was not accepted");
    checkSelection();
    const std::vector<std::uint8_t> cutoffUpdate { 0xf0, 0x41, 0x32, 0x00, 0x05, 87, 0xf7 };
    if (!patchParametersForEvent(cutoffUpdate, decoded, havePatch, selected, changed)
        || changed.cutoff != 87.0f / 127.0f)
        throw std::runtime_error("single-control update was not applied");
    checkSelection();
    havePatch = false;
    if (patchParametersForEvent(cutoffUpdate, decoded, havePatch, selected, changed))
        throw std::runtime_error("single-control update was accepted before an initial patch");
    if (!patchParametersForEvent(fullPatch, decoded, havePatch, cardProfile, changed)
        || !changed.useFixedVcfServiceFrequencyTrim || !changed.useServiced439522VcfCalibration
        || !patchParametersForEvent(cutoffUpdate, decoded, havePatch, cardProfile, changed)
        || !changed.useFixedVcfServiceFrequencyTrim || !changed.useServiced439522VcfCalibration)
        throw std::runtime_error("patch update lost the voice-card profile");
    std::istringstream highRateInput("0 903c7f\n0.1 803c00\n");
    if (readEvents(highRateInput, 192000u).back().frame != 19200u)
        throw std::runtime_error("event timing ignored the requested sample rate");
    YouKnowEngine rotaryEngine;
    rotaryEngine.prepare(48000, 1, 1);
    rotaryEngine.setParameters(parametersFor(sysex::Patch {}, 0.0f, true));
    for (int note = 0; note < 12; ++note)
    {
        YouKnowTestAccess::primeRotaryAssignment(rotaryEngine, note % 6, 60);
        rotaryEngine.noteOn(60, 1.0f);
        if (YouKnowTestAccess::heldSlot(rotaryEngine) != note % 6)
            throw std::runtime_error("test-mode fixture did not rotate through fixed cards");
        rotaryEngine.noteOff(60);
    }
    StereoBuffer tiny;
    tiny.left = {0.0f, .125f}; tiny.right = tiny.left;
    std::string writeError;
    const auto temporary = std::filesystem::temp_directory_path()
        / ("youknow-calibration-rate-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    if (!writeFloatWav(temporary, tiny, writeError, 192000u))
        throw std::runtime_error(writeError);
    std::array<std::uint8_t, 32> header {};
    std::ifstream rateFile(temporary, std::ios::binary);
    rateFile.read(reinterpret_cast<char*>(header.data()), header.size());
    rateFile.close();
    std::filesystem::remove(temporary);
    if (readU32(header.data() + 24) != 192000u
        || readU32(header.data() + 28) != 192000u * 8u)
        throw std::runtime_error("WAV header mislabeled the high-rate render");
    std::istringstream input("# timestamped MIDI\n0 903c7f\n0.05 803c00\n");
    const auto events = readEvents(input);
    if (events.size() != 2 || events[0].frame != 0 || events[1].frame != 2400
        || events[0].bytes != std::vector<std::uint8_t> { 0x90, 60, 127 })
        throw std::runtime_error("event parser changed the source timing or bytes");
    for (const auto* text : { "", "-1 903c7f\n", "nan 903c7f\n",
                             "1 903c7f\n0 803c00\n", "0 903c7\n",
                             "0 903cg0\n", "0 903c7f ignored\n" })
    {
        bool rejected = false;
        try { std::istringstream bad(text); (void) readEvents(bad); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected)
            throw std::runtime_error("invalid event input was accepted");
    }
    std::cout << "calibration event/options parser and patch-update persistence self-check passed\n";
}
} // namespace

int main(int argc, char** argv)
{
    const bool selfCheck = argc == 2 && std::string(argv[1]) == "--self-test";
    if (!selfCheck && (argc < 3 || argc > 12))
    {
        std::cerr << "usage: " << argv[0]
                  << " <seconds-hex-events.txt> <output.wav> [character 0..2]"
                     " [exact|shipping] [noise-scale 0..4] [nominal|a11-effective]"
                     " [dynamic|fixed-service] [nominal|serviced439522]"
                     " [sample-rate Hz] [oversample 1|2|4] [normal|rotary6]\n";
        return 2;
    }
    try
    {
        if (selfCheck)
        {
            selfTest();
            return 0;
        }
        const auto options = readOptions(std::vector<std::string>(argv + 3, argv + argc));
        std::ifstream input(argv[1]);
        if (!input)
            throw std::runtime_error("cannot open event file");
        const auto events = readEvents(input, options.sampleRate);
        YouKnowEngine engine;
        engine.selectConverterTimingProfile(
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine.prepare(options.sampleRate, comparisonBlockSize, options.oversampleFactor);
        sysex::Patch patch;
        bool havePatch = false;
        StereoBuffer audio;
        if (events.back().frame + 2 * options.sampleRate
            > (std::numeric_limits<std::uint32_t>::max() - 36u) / 8u)
            throw std::runtime_error("requested render exceeds RIFF/WAVE capacity");
        audio.left.resize(events.back().frame + 2 * options.sampleRate);
        audio.right.resize(audio.left.size());
        std::size_t cursor = 0;
        int rotarySlot = 0;
        const auto renderUntil = [&](std::size_t end) {
            while (cursor < end)
            {
                const auto count = std::min<std::size_t>(comparisonBlockSize, end - cursor);
                engine.process(audio.left.data() + cursor, audio.right.data() + cursor,
                               static_cast<int>(count));
                cursor += count;
            }
        };
        for (const auto& event : events)
        {
            renderUntil(event.frame);
            const auto& bytes = event.bytes;
            EngineParameters changed;
            if (patchParametersForEvent(bytes, patch, havePatch, options, changed))
                engine.setParameters(changed);
            else if (havePatch && bytes.size() == 3 && bytes[1] < 128 && bytes[2] < 128
                     && ((bytes[0] & 0xf0) == 0x80 || (bytes[0] & 0xf0) == 0x90))
            {
                if ((bytes[0] & 0xf0) == 0x90 && bytes[2] != 0)
                {
                    if (options.rotary)
                    {
                        YouKnowTestAccess::primeRotaryAssignment(engine, rotarySlot, bytes[1]);
                        rotarySlot = (rotarySlot + 1) % 6;
                    }
                    engine.noteOn(bytes[1], bytes[2] / 127.0f);
                }
                else
                    engine.noteOff(bytes[1]);
            }
            else
                throw std::runtime_error("unsupported MIDI or missing initial patch at frame "
                                         + std::to_string(event.frame));
        }
        renderUntil(audio.left.size());
        std::string error;
        if (!writeFloatWav(argv[2], audio, error, options.sampleRate))
            throw std::runtime_error(error);
        std::cout << events.size() << " events, "
                  << audio.left.size() / double(options.sampleRate)
                  << " seconds, character " << options.character
                  << ", " << options.sampleRate << " Hz/requested "
                  << options.oversampleFactor << "x, "
                  << "applied " << engine.getOversamplingFactor() << "x, "
                  << (options.shipping ? "Poly/Cubic/RK4 x1" : "Exact/Merson")
                  << ", noise scale " << options.noiseScale
                  << ", chorus " << (options.a11EffectiveChorus ? "A11 effective Mode I" : "nominal")
                  << ", VCF trim " << (options.fixedServiceTrim ? "fixed service" : "dynamic")
                  << ", VCF calibration " << (options.referenceVcf ? "serviced439522" : "nominal")
                  << ", allocation " << (options.rotary ? "test rotary6" : "normal")
                  << ", volume 1, peak "
                  << decibels(measure(audio).peak) << " dBFS\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
