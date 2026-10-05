// Combined-profile integration: real six-voice Engine processing exercises
// all five candidates together. This is a stability/lifecycle/callback test,
// not a fitted audio snapshot or evidence of original-unit sound accuracy.
#include "DSP/YouKnowProductHardwareRealism.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace
{
std::atomic<bool> guardAllocations { false };
std::atomic<unsigned> allocations { 0 };
}
void* operator new(std::size_t bytes)
{
    if (guardAllocations.load(std::memory_order_relaxed)) ++allocations;
    if (auto* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace youknow
{
struct YouKnowTestAccess
{
    static auto parameters(const YouKnowEngine& engine) { return engine.activeParameters_; }
    static bool coupledMixer(const YouKnowEngine& engine) { return engine.coupledMixerEnabled_; }
    static auto mixerCalibration(const YouKnowEngine& engine) { return engine.coupledMixerCalibration_; }
    static auto chorusTransfer(const YouKnowEngine& engine) { return engine.chorus_.getBbdTransferProfile(); }
    static auto chorusInsertion(const YouKnowEngine& engine) { return engine.chorus_.getBbdInsertionGainProfile(); }
    static auto chorusSupport(const YouKnowEngine& engine) { return engine.chorus_.getSupportProfile(); }
    static auto chorusBuilds(const YouKnowEngine& engine) { return engine.chorus_.supportBuildCount_; }
    static auto resonanceOffset(const YouKnowEngine& engine, int slot) { return engine.voices_[slot].filter.resonanceOffsetVolts; }
    static auto stageOffsets(const YouKnowEngine& engine, int slot) { return engine.voices_[slot].filter.offsetVoltage; }
};
}

namespace
{
using namespace youknow;
using Probe = YouKnowTestAccess;
constexpr int blockSize = 128;
constexpr std::array<int, 6> chord { 48, 55, 60, 64, 67, 72 };

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

// The accepted B profile is the product default, including consumers that
// include the active header without CMake definitions. An explicit 0 remains
// the controlled previous-profile comparison. Keep this independent of the
// combined hardware-profile audio checks below, which run in both builds.
#if !defined(YOUKNOW_HARDWARE_REALISM_CANDIDATE) || YOUKNOW_HARDWARE_REALISM_CANDIDATE
using ExpectedActiveProfile = ProductHardwareRealismProfile;
#else
using ExpectedActiveProfile = ProductFidelityProfile;
#endif
static_assert(std::is_same_v<ActiveProductFidelityProfile, ExpectedActiveProfile>,
              "Active product profile no longer follows accepted-B/default and explicit-0 policy");

void activeProductPromotion()
{
    EngineParameters activeParameters;
    activeParameters.volume = 0.37f;
    activeParameters.resonance = 0.52f;
    ActiveProductFidelityProfile::applyTo(activeParameters);
    require(activeParameters.volume == 0.37f && activeParameters.resonance == 0.52f,
            "profile promotion rewrote stored panel controls");

    auto active = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*active);
    active->setParameters(activeParameters);
    const auto applied = Probe::parameters(*active);
    require(applied.volume == 0.37f && applied.resonance == 0.52f,
            "active Engine setup rewrote stored panel controls");
#if !defined(YOUKNOW_HARDWARE_REALISM_CANDIDATE) || YOUKNOW_HARDWARE_REALISM_CANDIDATE
    require(Probe::coupledMixer(*active)
            && Probe::chorusTransfer(*active) == ChorusBbdTransferProfile::ServicedBiasEstimate
            && Probe::chorusInsertion(*active) == ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate
            && applied.chorusTimingProfile == ChorusTimingProfile::HardwareEvidence
            && applied.enableEvidenceVcaCalibration && applied.useOriginalCardVcfCalibration
            && applied.enableVoiceVcaJunctionTemperature && applied.useBa662AResonanceOffsetEstimate,
            "active product setup lost an accepted-B circuit selection");
#else
    require(!Probe::coupledMixer(*active)
            && Probe::chorusTransfer(*active) == ChorusBbdTransferProfile::Legacy
            && applied.chorusTimingProfile == ChorusTimingProfile::OwnerBlend
            && !applied.enableEvidenceVcaCalibration && !applied.useOriginalCardVcfCalibration
            && applied.useServiced439522VcfCalibration,
            "explicit-0 product setup no longer selects the previous circuits");
#endif
}

void selections(const YouKnowEngine& engine)
{
    const auto parameters = Probe::parameters(engine);
    require(parameters.chorusTimingProfile == ChorusTimingProfile::HardwareEvidence,
            "combined profile did not retain recording-derived chorus timing");
    require(parameters.enableEvidenceVcaCalibration
            && parameters.enableVoiceVcaJunctionTemperature
            && parameters.enableCoupledVoiceVcaControl
            && parameters.enableVoiceVcaServiceGain,
            "combined profile did not retain the quiet VCA circuit selection");
    require(parameters.useOriginalCardVcfCalibration
            && parameters.useBa662AResonanceOffsetEstimate
            && parameters.useCircuitDerivedResonanceShape
            && parameters.enableResonanceSoftJunction,
            "combined profile did not retain original-card resonance/drive selection");
    require(Probe::coupledMixer(engine), "combined profile did not configure the coupled WAVE/Sub network");
    const auto mixer = Probe::mixerCalibration(engine);
    require(mixer.valid() && mixer.diodeSlopeVolts > 0.0 && mixer.unipolarSawSource
            && mixer.sourceOhms > 4000.0 && mixer.sourceOhms < 6000.0
            && mixer.pinToCoreGain > 14.0 && mixer.pinToCoreGain < 15.0,
            "combined profile lost the nonlinear diode/source/load prior");
    require(std::abs(engine.oscillatorLevelScale() - 0.738f) < 1e-7f
            && std::abs(engine.pulseLevelScale() - 0.857f) < 1e-7f,
            "combined profile did not keep the stated oscillator/pulse source levels");
    require(Probe::chorusTransfer(engine) == ChorusBbdTransferProfile::ServicedBiasEstimate
            && Probe::chorusInsertion(engine) == ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate
            && Probe::chorusSupport(engine) == ChorusSupportProfile::Nominal2SA1015Nonlinear,
            "combined profile lost the BBD operating prior or nonlinear support");
}

void resonanceOffsetEstimate()
{
    // Keep the physical card draw and IR3109 offsets fixed while changing the
    // named BA662 prior. The original card's 100k/1.5k return divider converts
    // 250 uV at the differential pair to at most 16.667 mV at the loop node.
    auto engine = std::make_unique<YouKnowEngine>();
    EngineParameters parameters;
    parameters.calibration = 1;
    parameters.useBa662AResonanceOffsetEstimate = false;
    engine->setParameters(parameters);
    engine->prepare(48000, blockSize, 1);
    std::array<float, 6> previous {};
    std::array<std::array<float, 4>, 6> stages {};
    for (int slot = 0; slot < 6; ++slot)
    {
        previous[slot] = Probe::resonanceOffset(*engine, slot);
        stages[slot] = Probe::stageOffsets(*engine, slot);
    }
    parameters.useBa662AResonanceOffsetEstimate = true;
    engine->setParameters(parameters);
    for (int slot = 0; slot < 6; ++slot)
    {
        const auto offset = Probe::resonanceOffset(*engine, slot);
        require(std::abs(offset) <= 0.000250f * (100000.0f / 1500.0f),
                "BA662 estimate exceeded its declared pair-voltage span");
        require(std::abs(offset * 6 - previous[slot]) < 1e-7f,
                "BA662 prior edit changed its deterministic card identity");
        require(Probe::stageOffsets(*engine, slot) == stages[slot],
                "BA662 estimate incorrectly rescaled IR3109 stage offsets");
    }
    parameters.calibration = 0;
    engine->setParameters(parameters);
    for (int slot = 0; slot < 6; ++slot)
        require(Probe::resonanceOffset(*engine, slot) == 0,
                "BA662 dispersion remained at calibrated nominal Character");
}

EngineParameters panel()
{
    EngineParameters parameters;
    ProductHardwareRealismProfile::applyTo(parameters);
    parameters.vcfTanhMode = VcfTanhMode::PolyZoned;
    parameters.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    parameters.vcfSolverMode = VcfSolverMode::Rk4Single;
    parameters.polyphony = 6;
    parameters.calibration = 1.0f;
    parameters.aging = 0.5f;
    parameters.velocityDepth = 0.0f;
    parameters.sawEnabled = parameters.pulseEnabled = true;
    parameters.subLevel = 0.65f;
    parameters.noiseLevel = 0.05f;
    parameters.pwmSource = PwmSource::Manual;
    parameters.pwmDepth = 0.25f;
    parameters.cutoff = 0.65f;
    parameters.resonance = 0.3f;
    parameters.envDepth = parameters.keyFollow = 0.0f;
    parameters.vcfLfoDepth = parameters.dcoLfoDepth = 0.0f;
    parameters.vcaMode = VcaMode::Gate;
    parameters.vcaLevel = 1.0f;
    parameters.attack = 0.0f;
    parameters.decay = 0.1f;
    parameters.sustain = 0.65f;
    parameters.release = 0.0f;
    parameters.highPass = HighPassMode::One;
    parameters.chorus = ChorusMode::One;
    parameters.chorusNoise = 0.0f;
    parameters.volume = 0.3f;
    parameters.outputRoute = HeadphoneOutput::Route::Line;
    parameters.outputSelector = OutputNetwork::Selector::High;
    parameters.outputLoadOhms = 10000.0f;
    parameters.outputCapacitancePf = 480.0f;
    parameters.outputMono = false;
    return parameters;
}

struct Measurements
{
    double energy {}, peak {}, stereoEnergy {};
    int frames {};
};
Measurements render(YouKnowEngine& engine, int frames)
{
    Measurements result;
    std::array<float, blockSize> left {}, right {};
    while (result.frames < frames)
    {
        const int count = std::min(blockSize, frames - result.frames);
        guardAllocations = true;
        engine.process(left.data(), right.data(), count);
        guardAllocations = false;
        for (int sample = 0; sample < count; ++sample)
        {
            require(std::isfinite(left[sample]) && std::isfinite(right[sample]),
                    "all-five Engine path produced nonfinite LINE audio");
            result.peak = std::max({ result.peak, std::abs(double(left[sample])),
                                    std::abs(double(right[sample])) });
            result.energy += double(left[sample]) * left[sample]
                           + double(right[sample]) * right[sample];
            const double side = double(left[sample]) - right[sample];
            result.stereoEnergy += side * side;
        }
        result.frames += count;
    }
    require(result.peak < 1.0, "combined-profile fixture clipped digital LINE output");
    require(allocations == 0, "combined-profile audio callback allocated");
    return result;
}

void notesOn(YouKnowEngine& engine)
{
    for (int note : chord) engine.noteOn(note, 1.0f);
    require(engine.getActiveVoiceCount() == 6, "fixture failed to activate six physical voices");
}

void rate(double sampleRate, bool qualityChange)
{
    auto engine = std::make_unique<YouKnowEngine>();
    ProductHardwareRealismProfile::configureBeforePrepare(*engine);
    require(engine->configureThermalStart(true), "settled thermal start rejected");
    auto parameters = panel();
    engine->setParameters(parameters);
    engine->prepare(sampleRate, blockSize, 1);
    selections(*engine);
    require(!engine->configureChorusBbdTransferProfile(ChorusBbdTransferProfile::Legacy),
            "prepared combined profile allowed its BBD prior to be changed");
    allocations = 0;

    // Let the real delayed wet gate admit the return, then play the chord.
    (void) render(*engine, static_cast<int>(sampleRate * 0.125));
    notesOn(*engine);
    const auto ordinary = render(*engine, static_cast<int>(sampleRate * 0.14));
    require(ordinary.energy > 1e-6 && ordinary.peak > 1e-4,
            "combined-profile chord is unexpectedly silent");
    require(ordinary.stereoEnergy > ordinary.energy * 1e-5,
            "real chorus returns did not contribute stereo audio");

    // Sane panel extrema stress the coupled mixer and original-card resonator
    // together, while modest main VOLUME retains LINE playback headroom.
    parameters.cutoff = parameters.resonance = parameters.subLevel = 1.0f;
    parameters.pwmDepth = 0.95f;
    parameters.noiseLevel = 0.2f;
    parameters.chorus = ChorusMode::Two;
    engine->setParameters(parameters);
    const auto extreme = render(*engine, static_cast<int>(sampleRate * 0.085));
    require(extreme.energy > 1e-6, "extreme panel fixture stopped sounding");

    parameters.vcaMode = VcaMode::Envelope;
    parameters.sustain = 0.02f;
    parameters.decay = 0.0f;
    parameters.cutoff = 0.45f;
    parameters.resonance = 0.4f;
    parameters.release = 0.1f;
    engine->setParameters(parameters);
    (void) render(*engine, static_cast<int>(sampleRate * 0.06));
    for (int note : chord) engine->noteOff(note);
    (void) render(*engine, static_cast<int>(sampleRate * 0.06));
    selections(*engine);

    engine->resetForHostStop();
    selections(*engine);
    engine->reset();
    selections(*engine);
    require(engine->getActiveVoiceCount() == 0, "cold reset retained a sounding voice");

    if (qualityChange)
    {
        parameters.chorus = ChorusMode::Off;
        engine->setParameters(parameters);
        const auto builds = Probe::chorusBuilds(*engine);
        (void) engine->setOversamplingFactor(2);
        (void) render(*engine, static_cast<int>(sampleRate * 0.16));
        require(engine->getOversamplingFactor() == 2, "idle quality request did not complete");
        require(Probe::chorusBuilds(*engine) == builds,
                "live quality change rebuilt chorus support on the audio thread");
        selections(*engine);
        parameters.chorus = ChorusMode::Two;
        engine->setParameters(parameters);
        notesOn(*engine);
        const auto changed = render(*engine, static_cast<int>(sampleRate * 0.08));
        require(changed.energy > 1e-8, "combined profile stopped sounding after quality change");
        selections(*engine);
    }

    std::cout << "all-five profile rate=" << sampleRate << "Hz chordPeak=" << ordinary.peak
              << " extremePeak=" << extreme.peak << " allocations=" << allocations << '\n';
}
}

int main()
{
    try
    {
        activeProductPromotion();
        resonanceOffsetEstimate();
        rate(44100.0, false);
        rate(48000.0, true);
        rate(96000.0, false);
        std::cout << "Hardware realism combined profile checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        guardAllocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
