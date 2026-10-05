// Public-evidence candidate qualification. Harmonics are measured by direct
// Fourier projection of the transfer, without importing its Hermite knots,
// curvature or interpolation algorithm. Targets are Panasonic's MN3009
// printed p.38 THD-Vbias/THD-Vi graph readings; the lifecycle/edge tests
// establish that the selected prior reaches the actual BBD capture path.
// https://www.ka-electronics.com/images/pdf/Panasonic_BBD.pdf#page=40
// Roland's p.19 adjustment selects symmetrical clipping, not a random offset:
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf#page=19
#include "DSP/YouKnowChorus.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>

namespace
{
std::atomic<bool> countAllocations { false };
std::atomic<unsigned> allocations { 0 };
}
void* operator new(std::size_t bytes)
{
    if (countAllocations.load(std::memory_order_relaxed)) ++allocations;
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
    static float transfer(float input, ChorusBbdTransferProfile profile)
    {
        return profile == ChorusBbdTransferProfile::Legacy
            ? Chorus::bbdTransfer(input) : Chorus::bbdServicedBiasTransfer(input);
    }
    static auto settings(ChorusMode mode, ChorusTimingProfile profile)
    { return Chorus::settingsFor(mode, profile); }
    static void capture(Chorus& chorus, float input)
    {
        // Constant history isolates the real edge write from host-grid input
        // interpolation. At one full clock period both lines acquire input.
        for (auto* line : { &chorus.lineA_, &chorus.lineB_ })
        {
            line->previousInput = line->previousInput2 = line->previousInput3 = input;
            (void) line->processClockedCore(input, 40000.0, 40000.0f, 0.0f);
        }
    }
    static float bucket(const Chorus& chorus, bool right)
    {
        const auto& line = right ? chorus.lineB_ : chorus.lineA_;
        return line.cells[static_cast<std::size_t>(line.writeIndex)];
    }
    static auto lineProfile(const Chorus& chorus, bool right)
    { return right ? chorus.lineB_.transferProfile : chorus.lineA_.transferProfile; }
    static auto insertionProfile(const Chorus& chorus, bool right)
    { return right ? chorus.lineB_.insertionGainProfile : chorus.lineA_.insertionGainProfile; }
    static float core(Chorus& chorus, float input, double clock, float noise, bool right = false)
    {
        auto& line = right ? chorus.lineB_ : chorus.lineA_;
        return line.processClockedCore(input, clock, chorus.sampleRate_, noise);
    }
    static float wet(Chorus& chorus, float input, double clock, float noise = 0.0f,
                     bool right = false)
    {
        auto& line = right ? chorus.lineB_ : chorus.lineA_;
        return line.process(input, clock, chorus.sampleRate_,
            chorus.support_.exactOutputConnected, noise);
    }
    static float injectedPostSource(Chorus& chorus, float source)
    {
        // Independent routing reference: an externally scaled, reconstructed
        // source drives the existing physical postfilter. This chorus never
        // selects or executes the candidate insertion-gain mechanism.
        auto transition = chorus.support_.exactOutputConnected;
        transition.heldOutputMap.available = false;
        chorus.lineA_.held = source;
        return chorus.lineA_.process(0, 0, chorus.sampleRate_, transition, 0, false);
    }
    static bool transportMatches(const Chorus& first, const Chorus& second, bool right = false)
    {
        const auto& a = right ? first.lineB_ : first.lineA_;
        const auto& b = right ? second.lineB_ : second.lineA_;
        return a.cells == b.cells && a.writeIndex == b.writeIndex
            && a.clockPhase == b.clockPhase && a.transferState == b.transferState
            && a.noiseState == b.noiseState && a.transferNoiseState == b.transferNoiseState
            && a.previousTransferNoise == b.previousTransferNoise;
    }
    static auto postCurrent(const Chorus& chorus)
    { return chorus.lineA_.nonlinearOutput.previousCurrent; }
    static auto relativeLineGains(const Chorus& chorus)
    { return std::array { chorus.lineGainA_, chorus.lineGainB_ }; }
};
}

namespace
{
using youknow::Chorus;
using youknow::ChorusBbdTransferProfile;
using youknow::ChorusBbdInsertionGainProfile;
using youknow::ChorusMode;
using youknow::ChorusSupportProfile;
using youknow::ChorusTimingProfile;
using Probe = youknow::YouKnowTestAccess;
constexpr auto legacy = ChorusBbdTransferProfile::Legacy;
constexpr auto serviced = ChorusBbdTransferProfile::ServicedBiasEstimate;
constexpr auto unityInsertion = ChorusBbdInsertionGainProfile::UnityReference;
constexpr auto siblingInsertion = ChorusBbdInsertionGainProfile::HoltersParkerJuno60Estimate;
constexpr double voltsPerUnit = 2.6;

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

struct Spectrum
{
    double thd {}, second {}, fundamentalGain {};
};
Spectrum spectrum(double inputVrms, ChorusBbdTransferProfile profile)
{
    constexpr int frames = 32768;
    std::array<std::complex<double>, 32> harmonics {};
    const double amplitude = std::sqrt(2.0) * inputVrms / voltsPerUnit;
    for (int frame = 0; frame < frames; ++frame)
    {
        const double phase = 2.0 * std::numbers::pi * frame / frames;
        const double sample = Probe::transfer(static_cast<float>(amplitude * std::sin(phase)), profile);
        for (std::size_t harmonic = 1; harmonic < harmonics.size(); ++harmonic)
            harmonics[harmonic] += sample * std::polar(1.0, -phase * harmonic);
    }
    double higherPower = 0.0;
    for (std::size_t harmonic = 2; harmonic < harmonics.size(); ++harmonic)
        higherPower += std::norm(harmonics[harmonic]);
    return { std::sqrt(higherPower / std::norm(harmonics[1])),
             std::abs(harmonics[2] / harmonics[1]),
             2.0 * std::abs(harmonics[1]) / (frames * amplitude) };
}

void measuredHarmonics()
{
    const auto old = spectrum(0.78, legacy);
    const auto nominal = spectrum(0.78, serviced);
    const auto loud = spectrum(2.0, serviced);
    // The 0.46% graph reading includes an unidentified noise contribution.
    // Conditional power subtraction of the documented 1.1-1.4 mVrms
    // wideband bracket predicts .4235-.4378% harmonics at 0.78 Vrms. Check
    // that evidence bracket rather than asserting falsely precise hardware THD.
    require(nominal.thd > 0.004235 && nominal.thd < 0.004379,
            "serviced-bias harmonics miss the conditional noise-deembedded bracket");
    require(std::abs(old.thd - 0.003) < 0.000002,
            "legacy 0.3 percent THD anchor changed");
    require(nominal.thd > 1.4 * old.thd,
            "serviced-bias candidate did not change the low-level distortion");
    require(loud.thd > 0.02 && loud.thd < 0.025,
            "high-level THD left the approximate Panasonic graph bracket");
    require(nominal.second < 1e-8 && loud.second < 1e-8,
            "service-balanced model invents even-harmonic bias asymmetry");
    require(spectrum(0.001, serviced).fundamentalGain > 0.999999,
            "new transfer changes the small-signal gain instead of distortion");
    std::cout << std::setprecision(9)
              << "BBD THD at0.78Vrms legacy=" << old.thd * 100
              << "% serviced=" << nominal.thd * 100
              << "% at2Vrms=" << loud.thd * 100 << "%\n";
}

void symmetryAndHeadroom()
{
    float previous = 0.0f;
    for (int index = 0; index <= 32000; ++index)
    {
        const float input = 8.0f * index / 32000.0f;
        const float output = Probe::transfer(input, serviced);
        require(std::isfinite(output) && output >= previous && output <= 1.1246614f,
                "serviced transfer is nonmonotonic or exceeds retained headroom");
        require(Probe::transfer(-input, serviced) == -output,
                "serviced transfer lost balanced clipping symmetry");
        previous = output;
    }
    const float maximum = std::numeric_limits<float>::max();
    require(Probe::transfer(maximum, serviced) == Probe::transfer(maximum, legacy)
            && Probe::transfer(-maximum, serviced) == -Probe::transfer(maximum, serviced),
            "candidate changed the finite extreme clipping ceiling");
    require(Probe::transfer(std::numeric_limits<float>::infinity(), serviced) == 0.0f
            && Probe::transfer(std::numeric_limits<float>::quiet_NaN(), serviced) == 0.0f,
            "nonfinite serviced transfer input is not contained");
}

void actualCaptureAndLifecycle()
{
    auto original = std::make_unique<Chorus>();
    auto candidate = std::make_unique<Chorus>();
    require(original->getBbdTransferProfile() == legacy, "raw chorus default changed");
    require(!candidate->configureBbdTransferProfile(static_cast<ChorusBbdTransferProfile>(77)),
            "invalid transfer profile accepted");
    require(candidate->configureBbdTransferProfile(serviced), "pre-prepare profile rejected");
    original->prepare(192000);
    candidate->prepare(192000);
    require(!candidate->configureBbdTransferProfile(legacy), "live operating point was reinterpreted");
    constexpr float input = 0.7f;
    allocations = 0;
    countAllocations = true;
    Probe::capture(*original, input);
    Probe::capture(*candidate, input);
    countAllocations = false;
    require(allocations == 0, "BBD candidate capture allocates");
    for (bool right : { false, true })
    {
        require(Probe::bucket(*original, right) == Probe::transfer(input, legacy),
                "legacy edge write changed");
        require(Probe::bucket(*candidate, right) == Probe::transfer(input, serviced)
                && Probe::bucket(*candidate, right) != Probe::bucket(*original, right),
                "selected prior does not reach a real BBD capture");
    }
    candidate->reset();
    require(!candidate->configureBbdTransferProfile(legacy), "reset bypasses profile lifecycle");
    candidate->prepare(384000, true);
    require(candidate->getBbdTransferProfile() == serviced
            && Probe::lineProfile(*candidate, false) == serviced
            && Probe::lineProfile(*candidate, true) == serviced,
            "rate change/reset discarded a line operating prior");
}

std::unique_ptr<Chorus> insertionDevice(double rate, ChorusBbdInsertionGainProfile gain,
                                       ChorusSupportProfile support)
{
    auto chorus = std::make_unique<Chorus>();
    require(chorus->configureBbdInsertionGainProfile(gain), "insertion-gain selection rejected");
    require(chorus->configureSupportProfile(support), "support selection rejected");
    require(chorus->configureBbdTransferProfile(serviced), "serviced transfer selection rejected");
    chorus->prepare(rate);
    return chorus;
}

void knownClockInsertionGain()
{
    // Use the complete clocked signal/transfer-loss/reconstruction/postfilter
    // path at the datasheet's 40 kHz clock. A 1 kHz low-level projection
    // tests absolute gain without fitting a waveform or removing clock images.
    const double expectedGain = std::pow(10.0, 2.3 / 20.0);
    for (const double rate : { 48000.0, 192000.0 })
    {
        auto unity = insertionDevice(rate, unityInsertion, ChorusSupportProfile::Nominal2SA1015);
        auto candidate = insertionDevice(rate, siblingInsertion, ChorusSupportProfile::Nominal2SA1015);
        std::array<std::complex<double>, 2> originalFundamental {}, candidateFundamental {};
        const int settle = static_cast<int>(rate * .08);
        const int frames = static_cast<int>(rate * .05);
        for (int frame = 0; frame < settle + frames; ++frame)
        {
            const double phase = 2.0 * std::numbers::pi * 1000.0 * frame / rate;
            const float input = static_cast<float>(.005 * std::sin(phase));
            for (std::size_t line = 0; line < 2; ++line)
            {
                const double original = Probe::wet(*unity, input, 40000.0, 0.0f, line != 0);
                const double changed = Probe::wet(*candidate, input, 40000.0, 0.0f, line != 0);
                require(std::isfinite(original) && std::isfinite(changed), "gain tone became nonfinite");
                if (frame >= settle)
                {
                    const auto oscillator = std::polar(1.0, -phase);
                    originalFundamental[line] += original * oscillator;
                    candidateFundamental[line] += changed * oscillator;
                }
            }
        }
        for (std::size_t line = 0; line < 2; ++line)
        {
            const auto ratio = candidateFundamental[line] / originalFundamental[line];
            require(std::abs(std::abs(ratio) - expectedGain) < 2e-6
                    && std::abs(std::arg(ratio)) < 2e-6,
                    "40 kHz BBD gain did not reach both complete postfilter paths");
        }
        require(Probe::transportMatches(*unity, *candidate)
                && Probe::transportMatches(*unity, *candidate, true),
                "insertion gain changed capture, transfer-loss or random chronology");
    }
}

void insertionGainPrecedesFollowerDrive()
{
    // 48 kHz uses the established BLEP/cubic reconstruction. Independently
    // scale a unity core's signal and inject it before the nonlinear followers;
    // compare this physical drive with the selected candidate's actual path.
    // Scaling a finished wet return cannot match this reference at loud drive.
    constexpr double rate = 48000.0;
    const float gain = static_cast<float>(std::pow(10.0, 2.3 / 20.0));
    constexpr auto nonlinear = ChorusSupportProfile::Nominal2SA1015Nonlinear;
    auto source = insertionDevice(rate, unityInsertion, nonlinear);
    auto reference = insertionDevice(rate, unityInsertion, nonlinear);
    auto original = insertionDevice(rate, unityInsertion, nonlinear);
    auto candidate = insertionDevice(rate, siblingInsertion, nonlinear);
    double maximumRoutingError = 0.0, maximumFinishedGainError = 0.0;
    double maximumCurrentDifference = 0.0;
    allocations = 0;
    countAllocations = true;
    for (int frame = 0; frame < 12000; ++frame)
    {
        const double phase = 2.0 * std::numbers::pi * 997.0 * frame / rate;
        const float input = static_cast<float>(.5 * std::sin(phase));
        const float reconstructed = Probe::core(*source, input, 40000.0, 0.0f);
        const float expected = Probe::injectedPostSource(*reference, reconstructed * gain);
        const float baseline = Probe::wet(*original, input, 40000.0);
        const float output = Probe::wet(*candidate, input, 40000.0);
        if (frame > 4000)
        {
            maximumRoutingError = std::max(maximumRoutingError,
                std::abs(static_cast<double>(output) - expected));
            maximumFinishedGainError = std::max(maximumFinishedGainError,
                std::abs(static_cast<double>(output) - baseline * gain));
            const auto a = Probe::postCurrent(*original);
            const auto b = Probe::postCurrent(*candidate);
            for (std::size_t port = 0; port < a.size(); ++port)
                maximumCurrentDifference = std::max(maximumCurrentDifference, std::abs(a[port] - b[port]));
        }
    }
    countAllocations = false;
    require(allocations == 0, "insertion-gain signal path allocates");
    require(maximumRoutingError < 2e-6,
            "candidate gain misses the independently scaled pre-follower source");
    require(maximumFinishedGainError > 1e-5 && maximumCurrentDifference > 1e-8,
            "insertion gain acts only on finished output instead of nonlinear follower drive");
    std::cout << "Insertion routing max-error=" << maximumRoutingError
              << " versus finished-output gain=" << maximumFinishedGainError << '\n';
}

void insertionNoiseAndLifecycle()
{
    // The physical source is output-referred. Its raw held process and both
    // past/future BLEP noise steps must be identical when no signal is present,
    // across the two seeds, clocks and exact-held/host-grid reconstruction.
    for (const double rate : { 48000.0, 192000.0 })
        for (const double clock : { 40000.0, 100000.0 })
        {
            auto unity = insertionDevice(rate, unityInsertion, ChorusSupportProfile::Nominal2SA1015);
            auto candidate = insertionDevice(rate, siblingInsertion, ChorusSupportProfile::Nominal2SA1015);
            for (int frame = 0; frame < 1200; ++frame)
                for (bool right : { false, true })
                {
                    const float a = Probe::core(*unity, 0.0f, clock, 1.0f, right);
                    const float b = Probe::core(*candidate, 0.0f, clock, 1.0f, right);
                    require(std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b),
                            "signal insertion gain scaled output-referred noise or its BLEP prediction");
                }
            require(Probe::transportMatches(*unity, *candidate)
                    && Probe::transportMatches(*unity, *candidate, true),
                    "signal gain changed noise-generator history");
        }
    auto candidate = std::make_unique<Chorus>();
    require(candidate->getBbdInsertionGainProfile() == unityInsertion,
            "raw chorus insertion reference changed");
    require(!candidate->configureBbdInsertionGainProfile(static_cast<ChorusBbdInsertionGainProfile>(77)),
            "invalid insertion prior accepted");
    require(candidate->configureBbdInsertionGainProfile(siblingInsertion),
            "pre-prepare insertion prior rejected");
    candidate->prepare(192000.0);
    require(!candidate->configureBbdInsertionGainProfile(unityInsertion),
            "live insertion prior was reinterpreted");
    candidate->reset();
    candidate->prepare(384000.0, true);
    require(candidate->getBbdInsertionGainProfile() == siblingInsertion
            && Probe::insertionProfile(*candidate, false) == siblingInsertion
            && Probe::insertionProfile(*candidate, true) == siblingInsertion,
            "reset or processing-rate change lost insertion-gain selection");
    auto unity = insertionDevice(384000.0, unityInsertion, ChorusSupportProfile::IdealFollowers);
    float left {}, right {};
    unity->process(.02f, ChorusMode::One, 0.0f, left, right,
                   false, false, .75f, false, true, false, true);
    candidate->process(.02f, ChorusMode::One, 0.0f, left, right,
                       false, false, .75f, false, true, false, true);
    require(Probe::relativeLineGains(*unity) == Probe::relativeLineGains(*candidate),
            "absolute insertion gain reinterprets the existing line-relative gain");
}

void evidenceTiming()
{
    const auto one = Probe::settings(ChorusMode::One, ChorusTimingProfile::HardwareEvidence);
    const auto fit = Probe::settings(ChorusMode::One, ChorusTimingProfile::A11Spectral);
    const auto two = Probe::settings(ChorusMode::Two, ChorusTimingProfile::HardwareEvidence);
    require(one.rateHz == fit.rateHz && one.centreDelaySeconds == fit.centreDelaySeconds
            && one.sweepSeconds == fit.sweepSeconds && one.wetGain == fit.wetGain,
            "HardwareEvidence changed the verified effective Mode-I fit");
    require(std::abs(one.rateHz - 0.5159334275) < 1e-7
            && std::abs(one.centreDelaySeconds - 0.00338027575) < 3e-10
            && std::abs(one.sweepSeconds - 0.00176176683) < 2e-10,
            "recording-derived timing coordinates drifted");
    // Independent ratio of nominal effective timing resistances, derived
    // from the p.15 switched T-network (same thresholds and capacitor).
    const double ratio = 6435294.117647059 / 3963888.888888889;
    require(std::abs(two.rateHz / one.rateHz - ratio) < 2e-7
            && two.centreDelaySeconds == one.centreDelaySeconds
            && two.sweepSeconds == one.sweepSeconds && two.wetGain == one.wetGain,
            "Mode-II estimate no longer follows the shared-depth schematic");
    const auto extension = Probe::settings(ChorusMode::OneTwo, ChorusTimingProfile::HardwareEvidence);
    const auto oldExtension = Probe::settings(ChorusMode::OneTwo, ChorusTimingProfile::Shipping);
    require(extension.rateHz == oldExtension.rateHz
            && extension.centreDelaySeconds == oldExtension.centreDelaySeconds
            && extension.sweepSeconds == oldExtension.sweepSeconds,
            "one-unit profile reinterprets the I+II product extension");
    std::cout << "Evidence timing I=" << one.rateHz << "Hz II-estimate=" << two.rateHz
              << "Hz center=" << one.centreDelaySeconds * 1000
              << "ms depth=" << one.sweepSeconds * 1000 << "ms\n";
}
}

int main()
{
    try
    {
        measuredHarmonics();
        symmetryAndHeadroom();
        actualCaptureAndLifecycle();
        knownClockInsertionGain();
        insertionGainPrecedesFollowerDrive();
        insertionNoiseAndLifecycle();
        evidenceTiming();
        std::cout << "Chorus hardware-evidence candidates passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
