// Fresh audible-realism listening protocol, 2026-10-15, revision 2.
// Compile this SAME source against every build's OWN headers and Release DSP
// archive. Shipping A is immutable main 15a784c, with no candidate macro.
// Candidate builds define YOUKNOW_AUDIBLE_REALISM_R2 and
// YOUKNOW_AUDIBLE_REALISM_VARIANT: 0=all candidates off, 1=mixer, 2=chorus,
// 3=voice filter, 4=combined. These are three mechanisms, NOT seven fixes.
// Seven scores expose them under different musical conditions. Only variant 4
// is grouped evidence. Verify variant 0 against shipping A before isolation.
//
// Active product circuitry, Original firmware timing, PolyZoned/Cubic/Rk4Single,
// Character 1, Aging 0.5, and legal panel PWM coordinates are identical across
// letters. Default rendering is 48 kHz / 1x, fixed deterministic engine/card/noise
// reset seeds, a cold start plus 3 seconds of discarded idle preroll, and at most
// 128 frames per callback, split exactly at integer-frame events. There is one
// fresh engine per score, no reset inside a score, and no gain modification,
// silence removal or noise gating. The manifest enumerates every original
// EngineParameters field and every event; candidate flags are separate metadata.
//
// --render SCORE OUTPUT.wav [original|direct] [1|2|4] writes raw float32 stereo.
// --list lists scores. MakeFidelityQuickWins.py --renderer-source THIS_FILE
// --page-mode split packages matched PCM24 pairs and retains original raw gain.
// These compare software implementations; they are not hardware recordings,
// listening verdicts or a quantitative performance qualification.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <ctime>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifndef YOUKNOW_AUDIBLE_REALISM_VARIANT
#define YOUKNOW_AUDIBLE_REALISM_VARIANT 0
#endif

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr int rate = 48000;
constexpr int blockSize = 128;
constexpr int prerollFrames = 3 * rate;
constexpr int variant = YOUKNOW_AUDIBLE_REALISM_VARIANT;
static_assert(variant >= 0 && variant <= 4);
#if !defined(YOUKNOW_AUDIBLE_REALISM_R2)
static_assert(variant == 0, "A shipping build cannot select an unavailable candidate");
#endif
constexpr std::array<const char*, 7> names {
    "mixer-high-sub", "mixer-bright-stack", "chorus-one", "chorus-two",
    "filter-open", "filter-resonant-sweep", "combined-unison"
};

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void selectCandidate(EngineParameters& p)
{
#if defined(YOUKNOW_AUDIBLE_REALISM_R2)
    // Always override ALL three after the current active profile; a candidate
    // promotion must not silently turn an isolated B into a grouped comparison.
    p.enableCoupledMixerBandlimitedSwitch = variant == 1 || variant == 4;
    p.enableChorusRateIsland = variant == 2 || variant == 4;
    p.enableVoiceFilterRateIsland = variant == 3 || variant == 4;
#else
    (void) p;
#endif
}

struct Event
{
    int frame {};
    enum Kind { NoteOn, NoteOff, Patch } kind {};
    int note {};
    float value {};
    EngineParameters patch;
};

struct Score
{
    EngineParameters initial;
    std::vector<Event> events;
    int frames = 8 * rate;
    std::string description;

    void note(double start, double duration, int pitch, float velocity = 1.0f)
    {
        events.push_back({static_cast<int>(std::llround(start * rate)), Event::NoteOn, pitch, velocity, {}});
        events.push_back({static_cast<int>(std::llround((start + duration) * rate)), Event::NoteOff, pitch, 0.0f, {}});
    }
    void patch(double time, const EngineParameters& p)
    {
        events.push_back({static_cast<int>(std::llround(time * rate)), Event::Patch, 0, 0.0f, p});
    }
};

Score scoreFor(const std::string& name)
{
    Score s;
    auto& p = s.initial;
    ActiveProductFidelityProfile::applyTo(p);
    selectCandidate(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1.0f; p.aging = 0.5f; p.polyphony = 6;
    p.sawEnabled = true; p.pulseEnabled = false;
    p.range = DcoRange::Eight; p.subLevel = 0.0f; p.noiseLevel = 0.0f;
    p.pwmDepth = 0.0f; p.pwmSource = PwmSource::Manual;
    p.cutoff = 1.0f; p.resonance = 0.0f; p.envDepth = 0.0f; p.keyFollow = 0.0f;
    p.dcoLfoDepth = p.vcfLfoDepth = 0.0f;
    p.attack = 0.0f; p.decay = 0.15f; p.sustain = 0.85f; p.release = 0.10f;
    p.vcaLevel = 0.75f; p.volume = 0.40f;
    p.chorus = ChorusMode::Off; p.highPass = HighPassMode::One;
    p.velocityDepth = 0.0f;

    if (name == "mixer-high-sub")
    {
        s.description = "Sub only at 4-foot range: MIDI 84 and 96 are hardware-keyboard anchors; the third note, MIDI 108, is extended-MIDI stress beyond the hardware keyboard. An ordinary bass phrase follows. Listen for rough or inharmonic high-note components. Chorus off.";
        p.sawEnabled = false; p.subLevel = 1.0f; p.range = DcoRange::Four;
        s.note(0.1, 1.0, 84); s.note(1.4, 1.0, 96); s.note(2.7, 1.0, 108);
        auto q = p; q.range = DcoRange::Eight; s.patch(4.0, q);
        const std::array<int, 6> notes {36, 43, 48, 46, 43, 36};
        for (std::size_t i = 0; i < notes.size(); ++i) s.note(4.2 + i * 0.45, 0.30, notes[i]);
    }
    else if (name == "mixer-bright-stack")
    {
        s.description = "Saw, pulse and strong sub together: high single notes and a wide six-note stack. Listen to the upper-register texture. Chorus off.";
        p.pulseEnabled = true; p.pwmDepth = 0.62f; p.subLevel = 0.85f;
        s.note(0.1, 0.9, 84); s.note(1.3, 0.9, 91); s.note(2.5, 0.9, 96);
        for (int n : {48, 60, 67, 76, 84, 91}) s.note(3.9, 2.8, n);
    }
    else if (name == "chorus-one" || name == "chorus-two")
    {
        s.description = "Bright single notes followed by a held six-note stereo chord and natural tail. Listen for stable pitch and upper-partial texture while the chorus moves.";
        p.chorus = name == "chorus-one" ? ChorusMode::One : ChorusMode::Two;
        p.pulseEnabled = true; p.pwmDepth = 0.45f; p.subLevel = 0.15f;
        p.resonance = 0.05f; p.release = 0.20f;
        s.note(0.1, 1.0, 84); s.note(1.5, 1.0, 96);
        for (int n : {48, 55, 62, 69, 76, 83}) s.note(3.0, 3.4, n);
    }
    else if (name == "filter-open")
    {
        s.description = "Open filter with moderate resonance: bright upper-register notes and a six-note chord. Compare brightness and upper harmonics, as well as any unwanted roughness or inharmonic components. Chorus off.";
        p.pulseEnabled = true; p.pwmDepth = 0.30f; p.subLevel = 0.25f;
        p.resonance = 0.25f;
        s.note(0.1, 0.9, 72); s.note(1.3, 0.9, 84); s.note(2.5, 0.9, 96);
        for (int n : {60, 67, 72, 79, 84, 91}) s.note(3.9, 2.8, n);
    }
    else if (name == "filter-resonant-sweep")
    {
        s.description = "A held triad under a rising and falling resonant cutoff sweep. Listen to the moving resonance and any extra roughness. Chorus off.";
        p.pulseEnabled = true; p.pwmDepth = 0.40f; p.subLevel = 0.20f;
        p.cutoff = 0.15f; p.resonance = 0.78f; p.sustain = 1.0f;
        p.release = 0.15f;
        for (int n : {48, 55, 60}) s.note(0.2, 6.2, n);
        for (int step = 1; step <= 48; ++step)
        {
            auto q = p; q.cutoff = 0.15f + 0.85f * static_cast<float>(step) / 48.0f;
            s.patch(1.0 + 3.0 * step / 48.0, q);
        }
        for (int step = 1; step <= 32; ++step)
        {
            auto q = p; q.cutoff = 1.0f - 0.85f * static_cast<float>(step) / 32.0f;
            s.patch(4.0 + 2.0 * step / 32.0, q);
        }
    }
    else if (name == "combined-unison")
    {
        s.description = "Musical context: six-card Unison, repeated bass attacks, overlapping lead notes and a portamento return, with pulse, sub and chorus II. This final comparison combines all selected mechanisms.";
        p.keyMode = KeyMode::Unison; p.pulseEnabled = true;
        p.pwmDepth = 0.55f; p.pwmSource = PwmSource::Lfo; p.lfoRate = 0.45f;
        p.subLevel = 0.45f; p.cutoff = 0.72f; p.resonance = 0.40f; p.envDepth = 0.25f;
        p.chorus = ChorusMode::Two; p.release = 0.15f;
        for (int i = 0; i < 4; ++i) s.note(0.1 + i * 0.45, 0.28, 43);
        s.note(2.1, 2.0, 48); s.note(2.65, 0.9, 67); s.note(3.0, 0.3, 79);
        auto q = p; q.portamento = 0.30f; s.patch(4.3, q);
        s.note(4.5, 2.2, 43); s.note(5.2, 0.9, 79);
    }
    else throw std::runtime_error("unknown score: " + name);

    std::stable_sort(s.events.begin(), s.events.end(), [](const Event& a, const Event& b) {
        return a.frame < b.frame;
    });
    const auto checkPatch = [](const EngineParameters& patch) {
        require(patch.pwmDepth >= 0.0f && patch.pwmDepth <= YouKnowEngine::pwmPanelTopPosition,
                "score exceeds the shipping PWM panel range");
    };
    checkPatch(s.initial);
    for (const auto& event : s.events)
    {
        require(event.frame >= 0 && event.frame < s.frames, "event is outside the score");
        if (event.kind == Event::Patch) checkPatch(event.patch);
    }
    return s;
}

// Enumerate every EngineParameters field present in shipping 15a784c. This
// includes unchanged physical-profile coordinates, not merely visible controls.
// The three new mechanism selectors are separate manifest metadata so the
// packager can require every common parameter and event to be identical.
void patchJson(std::ostream& out, const EngineParameters& p)
{
    out << '{';
    bool first = true;
    const auto field = [&](const char* name, double value) {
        if (!first) out << ',';
        first = false;
        out << std::quoted(name) << ':' << value;
    };
#define PATCH_FIELD(name) field(#name, static_cast<double>(p.name))
    PATCH_FIELD(lfoRate);
    PATCH_FIELD(lfoSyncRateHz);
    PATCH_FIELD(lfoSyncDelaySeconds);
    PATCH_FIELD(allowHostMasterTuneExtension);
    PATCH_FIELD(lfoDelay);
    PATCH_FIELD(dcoLfoDepth);
    PATCH_FIELD(pwmDepth);
    PATCH_FIELD(pwmSource);
    PATCH_FIELD(range);
    PATCH_FIELD(sawEnabled);
    PATCH_FIELD(pulseEnabled);
    PATCH_FIELD(subLevel);
    PATCH_FIELD(noiseLevel);
    PATCH_FIELD(highPass);
    PATCH_FIELD(cutoff);
    PATCH_FIELD(resonance);
    PATCH_FIELD(envPolarity);
    PATCH_FIELD(envDepth);
    PATCH_FIELD(vcfLfoDepth);
    PATCH_FIELD(keyFollow);
    PATCH_FIELD(vcaMode);
    PATCH_FIELD(vcaLevel);
    PATCH_FIELD(attack);
    PATCH_FIELD(decay);
    PATCH_FIELD(sustain);
    PATCH_FIELD(release);
    PATCH_FIELD(chorus);
    PATCH_FIELD(keyMode);
    PATCH_FIELD(portamento);
    PATCH_FIELD(keyTranspose);
    PATCH_FIELD(benderDcoDepth);
    PATCH_FIELD(benderVcfDepth);
    PATCH_FIELD(benderLfoDepth);
    PATCH_FIELD(masterTuneCents);
    PATCH_FIELD(volume);
    PATCH_FIELD(outputTrimDb);
    PATCH_FIELD(velocityDepth);
    PATCH_FIELD(calibration);
    PATCH_FIELD(chorusNoise);
    PATCH_FIELD(chorusNoiseCalibrationProfile);
    PATCH_FIELD(mainNoiseLevelScale);
    PATCH_FIELD(mainNoiseCalibrationProfile);
    PATCH_FIELD(polyphony);
    PATCH_FIELD(vcfTanhMode);
    PATCH_FIELD(vcfSolverMode);
    PATCH_FIELD(enableVcfStageOffsets);
    PATCH_FIELD(enableResonanceOtaOffset);
    PATCH_FIELD(useBa662AResonanceOffsetEstimate);
    PATCH_FIELD(enableOpAmpSlewLimiting);
    PATCH_FIELD(enableVcfEarlyEffect);
    PATCH_FIELD(enableSpatialThermalGradient);
    PATCH_FIELD(enablePulseOffWaveNodeCoupling);
    PATCH_FIELD(enableSawUnipolarNodeCoupling);
    PATCH_FIELD(enableSubHalfWaveNodeCoupling);
    PATCH_FIELD(enableVoiceVcaSignalSaturation);
    PATCH_FIELD(enableVoiceVcaAntialias);
    PATCH_FIELD(enableVoiceVcaServiceGain);
    PATCH_FIELD(enableVoiceVcaTemperature);
    PATCH_FIELD(enableCoupledVoiceVcaControl);
    PATCH_FIELD(enableEvidenceVcaCalibration);
    PATCH_FIELD(enableVoiceVcaJunctionTemperature);
    PATCH_FIELD(enableNoiseLevelBeforeC41);
    PATCH_FIELD(enableMainNoiseMagnitudePole);
    PATCH_FIELD(useCircuitDerivedNoiseLevelShape);
    PATCH_FIELD(enableNoiseLevelSoftJunction);
    PATCH_FIELD(enableNarrowOneTwoChorus);
    PATCH_FIELD(enableChorusMuteDrive);
    PATCH_FIELD(enableChorusClockMuteCircuit);
    PATCH_FIELD(enableChorusFiniteMuteDrive);
    PATCH_FIELD(enableChorusFiniteTr5Drive);
    PATCH_FIELD(chorusTimingProfile);
    PATCH_FIELD(enableChorusCorrelatedNoise);
    PATCH_FIELD(chorusNoiseTransferFraction);
    PATCH_FIELD(chorusNoiseTransferCorrelation);
    PATCH_FIELD(useCircuitDerivedResonanceShape);
    PATCH_FIELD(enableResonanceSoftJunction);
    PATCH_FIELD(useFixedVcfServiceFrequencyTrim);
    PATCH_FIELD(useServiced439522VcfCalibration);
    PATCH_FIELD(useOriginalCardVcfCalibration);
    PATCH_FIELD(resonanceCompensationShape);
    PATCH_FIELD(enableCommonVcaNoise);
    PATCH_FIELD(enableCommonVcaControlNoise);
    PATCH_FIELD(enableCommonVcaOutputPole);
    PATCH_FIELD(enableOutputSummerMagnitudePole);
    PATCH_FIELD(enableOutputSummerAntialias);
    PATCH_FIELD(enableOutputSummerMuteLoading);
    PATCH_FIELD(enableCardJohnsonFloor);
    PATCH_FIELD(enableOtaShotNoise);
    PATCH_FIELD(enableBa662OutputMirrorNoise);
    PATCH_FIELD(enableBa662TailMirrorNoise);
    PATCH_FIELD(aging);
    PATCH_FIELD(outputRoute);
    PATCH_FIELD(headphoneLoadOhms);
    PATCH_FIELD(enableHeadphoneAmplifierDynamics);
    PATCH_FIELD(enableHeadphoneIntrinsicNoise);
    PATCH_FIELD(outputSelector);
    PATCH_FIELD(outputLoadOhms);
    PATCH_FIELD(outputCapacitancePf);
    PATCH_FIELD(outputMono);
    PATCH_FIELD(vcfFastEarlyMode);
    PATCH_FIELD(enableSubDiodeControl);
    PATCH_FIELD(enableSubStorageSkew);
    PATCH_FIELD(useMeasuredSubStorageSkew);
    PATCH_FIELD(enableResonanceHeadroomTemperature);
    PATCH_FIELD(enableResonanceServiceTrim);
    PATCH_FIELD(enableConverterHoldDroop);
    PATCH_FIELD(enableRailRipple);
#undef PATCH_FIELD
    out << '}';
}

void render(const std::string& name, const std::filesystem::path& path,
            bool original, int quality)
{
    require(!std::filesystem::exists(path), "output exists; preserve prior evidence");
    const auto score = scoreFor(name);
    auto engine = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    require(engine->configureThermalStart(false), "cannot select cold thermal start");
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(rate, blockSize, quality);
    engine->setParameters(score.initial);
    engine->setOriginalPerformanceMode(original);
    engine->setPitchBend(0.0f); engine->setModWheel(0.0f);
    std::array<float, blockSize> left {}, right {};
    double processCpuTicks = 0.0;
    const auto process = [&](int count) {
        const auto started = std::clock();
        engine->process(left.data(), right.data(), count);
        processCpuTicks += static_cast<double>(std::clock() - started);
        require(!original || engine->originalPerformanceHealthy(), "original firmware execution failed");
    };
    for (int frame = 0; frame < prerollFrames;)
    {
        const int count = std::min(blockSize, prerollFrames - frame);
        process(count); frame += count;
    }
    processCpuTicks = 0.0; // Preroll is excluded; diagnostic CPU only.
    StereoBuffer audio;
    audio.left.reserve(static_cast<std::size_t>(score.frames));
    audio.right.reserve(static_cast<std::size_t>(score.frames));
    std::size_t next = 0;
    int peakVoices = 0;
    for (int frame = 0; frame < score.frames;)
    {
        while (next < score.events.size() && score.events[next].frame == frame)
        {
            const auto& event = score.events[next++];
            switch (event.kind)
            {
                case Event::NoteOn: engine->noteOn(event.note, event.value); break;
                case Event::NoteOff: engine->noteOff(event.note); break;
                case Event::Patch: engine->setParameters(event.patch); break;
            }
        }
        int count = std::min(blockSize, score.frames - frame);
        if (next < score.events.size()) count = std::min(count, score.events[next].frame - frame);
        require(count > 0, "invalid score schedule");
        process(count);
        peakVoices = std::max(peakVoices, engine->getActiveVoiceCount());
        audio.left.insert(audio.left.end(), left.begin(), left.begin() + count);
        audio.right.insert(audio.right.end(), right.begin(), right.begin() + count);
        frame += count;
    }
    require(next == score.events.size(), "score contains events after render end");
    std::string error;
    require(validate(audio, error), error);
    const auto level = measure(audio);
    require(level.rms > 1e-7 && peakVoices > 0, "silent score");
    require(level.peak < 1.0, "raw score exceeds digital full scale; lower identical score controls before comparing");
    require(writeFloatWav(path, audio, error, rate), error);
    std::cout << std::setprecision(17)
        << "{\"protocol\":\"youknow-audible-realism-2026-10-15-v2\",\"score\":" << std::quoted(name)
        << ",\"description\":" << std::quoted(score.description)
        << ",\"sample_rate\":" << rate << ",\"channels\":2,\"frames\":" << score.frames
        << ",\"max_block_size\":" << blockSize << ",\"preroll_frames\":" << prerollFrames
        << ",\"quality_requested\":" << quality << ",\"quality_applied\":" << engine->getOversamplingFactor()
        << ",\"timing\":" << std::quoted(original ? "original" : "direct")
        << ",\"thermal_start\":\"cold\",\"seed_policy\":\"fixed engine reset/card/noise seeds\""
        << ",\"profile\":\"ActiveProductFidelityProfile\",\"kernel\":\"PolyZoned/Cubic/Rk4Single\""
        << ",\"candidate_variant\":" << variant
        << ",\"mechanism_flags\":{\"coupled_mixer\":" << (variant == 1 || variant == 4)
        << ",\"chorus_rate_island\":" << (variant == 2 || variant == 4)
        << ",\"voice_filter_rate_island\":" << (variant == 3 || variant == 4) << '}'
        << ",\"process_cpu_seconds\":" << processCpuTicks / CLOCKS_PER_SEC
        << ",\"peak\":" << level.peak << ",\"rms\":" << level.rms
        << ",\"peak_voices\":" << peakVoices << ",\"final_active_voices\":" << engine->getActiveVoiceCount()
        << ",\"patch\":";
    patchJson(std::cout, score.initial);
    std::cout << ",\"events\":[";
    bool first = true;
    for (const auto& event : score.events)
    {
        if (!first) std::cout << ',';
        first = false;
        constexpr std::array<const char*, 3> kinds {"on", "off", "patch"};
        std::cout << "{\"frame\":" << event.frame << ",\"kind\":" << std::quoted(kinds[event.kind]);
        if (event.kind == Event::Patch) { std::cout << ",\"patch\":"; patchJson(std::cout, event.patch); }
        else std::cout << ",\"note\":" << event.note << ",\"value\":" << event.value;
        std::cout << '}';
    }
    std::cout << "]}\n";
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--list")
        {
            for (const auto* name : names) std::cout << name << '\n';
            return 0;
        }
        require(argc >= 4 && argc <= 6 && std::string(argv[1]) == "--render",
            "usage: YouKnowRenderAudibleRealism --render SCORE OUTPUT.wav [original|direct] [1|2|4]; or --list");
        const std::string timing = argc > 4 ? argv[4] : "original";
        const std::string quality = argc > 5 ? argv[5] : "1";
        require(timing == "original" || timing == "direct", "timing must be original or direct");
        require(quality == "1" || quality == "2" || quality == "4", "quality must be 1, 2 or 4");
        render(argv[2], std::filesystem::absolute(argv[3]), timing == "original", quality[0] - '0');
    }
    catch (const std::exception& error)
    {
        std::cerr << "RenderAudibleRealism: " << error.what() << '\n';
        return 1;
    }
}
