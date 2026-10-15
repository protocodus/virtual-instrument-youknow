// A stateful VCF can reuse the VCA's existing FIR boundary only if audio,
// controls and noise all consume its SAME delayed local-rate intervals.
// These tests qualify that transport and its actual production-cascade
// small-signal response, plus the complete engine routing and lifecycle.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowVoiceRateIsland.h"
#include "DSP/YouKnowProductHardwareRealism.h"
#include <memory>
#include <cstdlib>

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace { bool guardAllocation = false; unsigned allocations = 0; }
void* operator new(std::size_t size) { if (guardAllocation) ++allocations; if (auto* p = std::malloc(size ? size : 1)) return p; throw std::bad_alloc{}; }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
namespace youknow
{
struct YouKnowTestAccess
{
    using Cascade = YouKnowEngine::OtaCascade;
    static auto parameters(const YouKnowEngine& e) { return e.activeParameters_; }
    static double integratedResponse(YouKnowEngine& e, double frequency) {
        auto p = e.activeParameters_; p.calibration = 0; e.setParameters(p);
        auto& v = e.voices_[0]; auto& card = e.cards_[0];
        card.johnsonTemperatureScale = 0;
        v.active = true; v.vca = 1; v.vcaInputTrim = 1;
        v.feedback = 0;
        constexpr double pi = 3.14159265358979323846, amplitude = .0001;
        YouKnowEngine::VoiceFilterFrame f;
        f.needsFilter = true; f.headroom = 6.366f;
        f.omegaStep = static_cast<float>(2*pi*50000/e.oversampledRate_);
        const int count = static_cast<int>(e.oversampledRate_/10);
        std::complex<double> projection {};
        for (int frame = 0; frame < 2*count; ++frame) {
            const double angle = 2*pi*frequency*frame/e.oversampledRate_;
            f.input = static_cast<float>(amplitude*std::sin(angle));
            const double y = e.renderVoiceFilterIsland(v, f);
            if (frame >= count) projection += y*std::polar(1.,-angle);
        }
        const auto& context = v.filterIsland.contexts[static_cast<std::size_t>(
            (v.filterIsland.frames-1)&31)];
        const double normalization = context.serviceGain*context.inputScale/2.6;
        return 20*std::log10(2*std::abs(projection)/(normalization*amplitude*count));
    }
    static auto contextBytes() { return sizeof(YouKnowEngine::VoiceFilterIslandContext); }
    static bool rebuild(YouKnowEngine& e, int factor) {
        for (auto& v : e.voices_) { v.active = false; v.dcoPitchTransactionValid = false; v.dco.pitWriteState = YouKnowEngine::Dco::PitWriteState::idle; }
        e.anyVoiceActive_ = false; e.oversamplingIdleSamples_ = e.oversamplingQuietSamples_;
        e.rateTransition_ = YouKnowEngine::RateTransition::FadingOut; e.rateTransitionGain_ = 0;
        return e.setOversamplingFactor(factor);
    }
    static void finite(const YouKnowEngine& e) {
        for (const auto& v : e.voices_) {
            for (double x : v.filter.state) if (!std::isfinite(x)) throw std::runtime_error("nonfinite physical filter state");
            if (!std::isfinite(v.vcaInputCoupling.state)) throw std::runtime_error("nonfinite C59 charge");
        }
    }
    static void coldNoiseAndControlChronology(YouKnowEngine& e) {
        auto& v = e.voices_[0];
        v.active = true; v.vca = .6f;
        auto expected = v.filterShotRandom;
        const auto before = v.filter.state;
        YouKnowEngine::VoiceFilterFrame f;
        f.needsFilter = true; f.input = .01f; f.headroom = 6; f.hasTrajectory = true;
        for (int frame = 0; frame < 96; ++frame) {
            for (std::size_t point = 0; point < 7; ++point) {
                const double position = Cascade::controlNodePositions[point];
                f.trajectory.omegaStep[point] = 1 + .001 * (frame - 1 + position);
                f.trajectory.feedback[point] = 0;
                f.trajectory.headroom[point] = 6;
            }
            f.omegaStep = static_cast<float>(f.trajectory.omegaStep.back());
            (void) e.renderVoiceFilterIsland(v, f);
            if (frame < 23 && v.filter.state != before)
                throw std::runtime_error("cold priming advanced physical capacitor state before timestamp0");
            for (int phase = 0; phase < 4; ++phase)
                if (frame - 24 + (phase == 0 ? 0 : 1) >= 0)
                    for (int draw = 0; draw < 4; ++draw) (void) expected.next();
            auto actual = v.filterShotRandom;
            auto copy = expected;
            if (actual.next() != copy.next())
                throw std::runtime_error("local filter shot-noise stream has wrong physical chronology");
            if (frame >= 24) {
                const double timestamp = frame - 24 + .75;
                const double expectedOmega = (1 + .001 * timestamp) / 4;
                if (std::abs(v.filter.previousOmegaStep - expectedOmega) > 2e-7)
                    throw std::runtime_error("local cascade consumed present-time or wrong-phase controls");
            }
        }
        const auto state = v.filter.state;
        const auto charge = v.vcaInputCoupling.state;
        e.resetVoiceFilterIsland(v, true);
        if (v.filter.state != state || v.vcaInputCoupling.state != charge)
            throw std::runtime_error("warm history rebuild reset physical charge");
        (void) e.renderVoiceFilterIsland(v, f);
        if (v.filter.state == state)
            throw std::runtime_error("warm history rebuild froze physical cascade");
    }
    static void unclippedInteriorControls(YouKnowEngine& e) {
        auto& v = e.voices_[0];
        e.activeParameters_.calibration = 0;
        v.islandControlPrimed = false;
        v.cutoffCounts = 15000; v.cutoffChainCounts = -1e30f;
        e.resonanceCv_ = 0;
        e.updateVoiceAudio(v, e.activeParameters_);
        e.exactVcfControlInterval_[0] = true;
        e.cutoffVcfHoldIntervals_[0].value.fill(15000);
        e.resonanceVcfHoldInterval_.value.fill(0);
        // A slight within-interval event curve above the base-grid cap must
        // survive the interior map, not only the endpoint cutoff memo.
        e.cutoffVcfHoldIntervals_[0].value[3] = 14500;
        const auto f = e.prepareVoiceFilter(v, e.activeParameters_, 0);
        if (!f.hasTrajectory || f.omegaStep <= .9 * 3.141592653589793)
            throw std::runtime_error("full-open candidate pole still clipped to base-grid Nyquist");
        if (f.trajectory.omegaStep[3] >= f.trajectory.omegaStep.back() - 1e-4)
            throw std::runtime_error("above-Nyquist interior control curvature clipped away");
    }

};
}

namespace
{
using namespace youknow;
constexpr double pi = 3.1415926535897932384626433832795;
void require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }

struct Context
{
    float gain {};
    double cutoff {};
    double marker {};
};
using Island = VoiceRateIsland<Context>;

void delayedControlParity()
{
    for (double rate : {44100., 48000., 96000., 192000.})
    {
        VoiceVcaAntialias reference, transport;
        Island island;
        const auto kernel = VoiceVcaAntialias::prepare(rate);
        std::uint32_t random = 0x3e2c1b7a;
        const auto next = [&] {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            return static_cast<float>(double(random) / 4294967295.0);
        };
        for (int frame = 0; frame < 4096; ++frame)
        {
            if (frame == 777)
            { reference.reset(); transport.reset(); island.reset(); }
            const float input = (next() - .5f) * 8;
            const float gain = frame % 71 < 11 ? 0.0f : next();
            const auto shape = [](float x) { return x / (1 + .01f * x * x); };
            const float expected = reference.process(input, gain, kernel, shape);
            unsigned calls = 0;
            const float actual = island.process(input, {gain, 0, double(frame)},
                kernel, transport, [&](float x, const Island::Interval& interval) {
                    ++calls;
                    require(interval.end - interval.begin == 1.0 / kernel.factor,
                        "local interval changed physical duration");
                    const double g = interval.interpolate(interval.previous.gain,
                                                          interval.current.gain);
                    return static_cast<float>(shape(x) * g);
                });
            require(calls == static_cast<unsigned>(kernel.factor),
                "local callback count lost a phase");
            require(std::bit_cast<std::uint32_t>(actual)
                        == std::bit_cast<std::uint32_t>(expected),
                "moving gain into the island changed the existing FIR/control output");
        }
    }
}

void intervalChronology()
{
    for (double rate : {48000., 96000., 192000.})
    {
        VoiceVcaAntialias transport;
        Island island;
        const auto kernel = VoiceVcaAntialias::prepare(rate);
        const int delay = kernel.factor == 1 ? 0 : Island::lookBack;
        double previousTime = -delay - 1.0 / kernel.factor;
        for (int frame = 0; frame < 256; ++frame)
        {
            (void) island.process(0, {1, 50000, double(frame)}, kernel, transport,
                [&](float, const Island::Interval& interval) {
                    const double time = interval.frame - 1 + interval.end;
                    require(std::abs(time - previousTime - 1.0 / kernel.factor) < 1e-12,
                        "phase-zero/next-interval ownership skipped or replayed time");
                    if (interval.frame >= 0)
                        require(interval.current.marker == interval.frame,
                            "physical control context came from the wrong frame");
                    if (interval.frame > 0)
                        require(interval.previous.marker == interval.frame - 1,
                            "previous physical context came from the wrong frame");
                    previousTime = time;
                    return 0.0f;
                });
        }
    }
}

void unchangedDelay()
{
    for (double rate : {44100., 48000., 96000., 192000.})
    {
        VoiceVcaAntialias transport;
        Island island;
        const auto kernel = VoiceVcaAntialias::prepare(rate);
        double sum = 0, weighted = 0;
        for (int frame = 0; frame < 256; ++frame)
        {
            const double output = island.process(frame == 0 ? 1.f : 0.f, {},
                kernel, transport, [](float x, const Island::Interval&) { return x; });
            sum += output; weighted += frame * output;
        }
        const double expected = kernel.factor == 1 ? 0 : VoiceVcaAntialias::delaySamples;
        require(std::abs(sum - 1) < 2e-6, "island transport changed DC gain");
        require(std::abs(weighted / sum - expected) < 2e-6,
            "island added or lost FIR latency");
    }
}

double response(double rate, double frequency, bool islandEnabled)
{
    VoiceVcaAntialias transport;
    Island island;
    YouKnowTestAccess::Cascade cascade;
    cascade.reset();
    cascade.gScale.fill(1);
    cascade.offsetVoltage.fill(0);
    cascade.inputCompensationCoefficient = 0;
    const auto kernel = VoiceVcaAntialias::prepare(rate);
    constexpr double cutoff = 50000, amplitude = .0001;
    const int count = static_cast<int>(rate / 10);
    std::complex<double> projection {};
    const auto solve = [&](float input, double localRate) {
        return cascade.process<true>(input,
            static_cast<float>(2 * pi * std::min(cutoff, .45 * localRate) / localRate),
            0, 6.366f, false, 0, nullptr, VcfTanhMode::PolyZoned,
            VcfSolverMode::Rk4Single);
    };
    for (int frame = 0; frame < count * 2; ++frame)
    {
        const double angle = 2 * pi * frequency * frame / rate;
        const float input = static_cast<float>(amplitude * std::sin(angle));
        const float output = islandEnabled
            ? island.process(input, {1, cutoff, 0}, kernel, transport,
                [&](float x, const Island::Interval& interval) {
                    return solve(x, rate * interval.factor);
                })
            : transport.process(solve(input, rate), 1, kernel,
                [](float x) { return x; });
        if (frame >= count)
            projection += static_cast<double>(output)
                * std::polar(1., -angle);
    }
    return 20 * std::log10(2 * std::abs(projection) / (amplitude * count));
}

void physicalCutoffResponse()
{
    for (double rate : {44100., 48000., 96000., 192000.})
        for (double frequency : {8000., 12000., 16000., 20000.})
        {
            const double actual = response(rate, frequency, true);
            const double analogue = -40 * std::log10(1 + std::pow(frequency / 50000, 2));
            // Includes the already documented FIR transition loss at44.1k;
            // 192k local ODE/input reconstruction contributes <.04dB.
            require(std::abs(actual - analogue) < .09,
                "local VCF missed the independent analogue four-pole response");
            if (rate == 48000.)
            {
                const double baseline = response(rate, frequency, false);
                require(std::abs(actual - analogue) < std::abs(baseline - analogue) / 25,
                    "moving the VCF did not remove the low-grid cutoff loss");
                std::cout << "VCF " << frequency << " Hz: baseline=" << baseline
                          << " dB island=" << actual << " dB analogue=" << analogue << " dB\n";
            }
        }
}
std::unique_ptr<YouKnowEngine> prepared(double rate, int factor, bool island)
{
    auto engine = std::make_unique<YouKnowEngine>();
    ProductHardwareRealismProfile::configureBeforePrepare(*engine);
    engine->prepare(rate, 64, factor);
    EngineParameters p;
    ProductHardwareRealismProfile::applyTo(p);
    p.enableVoiceFilterRateIsland = island;
    p.calibration = .7f; p.chorus = ChorusMode::Off;
    p.sawEnabled = p.pulseEnabled = true;
    p.cutoff = 1; p.resonance = .6f; p.attack = 0; p.sustain = 1;
    engine->setParameters(p);
    return engine;
}

void engineRoutingAndLifecycle()
{
    std::cout << "Engine bytes=" << sizeof(YouKnowEngine)
              << " context bytes=" << YouKnowTestAccess::contextBytes() << '\n';
    for (double rate : {176400., 192000., 384000.})
    {
        auto a = prepared(rate, 1, false), b = prepared(rate, 1, true);
        a->noteOn(72, 1); b->noteOn(72, 1);
        std::array<float, 64> al {}, ar {}, bl {}, br {};
        for (int block = 0; block < 24; ++block)
        {
            if (block == 8 || block == 16) {
                auto p = YouKnowTestAccess::parameters(*b);
                p.enableVoiceFilterRateIsland = !p.enableVoiceFilterRateIsland;
                b->setParameters(p);
            }
            a->process(al.data(), ar.data(), 64); b->process(bl.data(), br.data(), 64);
            require(al == bl && ar == br, "direct-grid diagnostic flag changed audio");
        }
        require(a->getProcessingLatencySamples() == b->getProcessingLatencySamples(),
                "direct-grid flag changed latency");
    }
    for (double rate : {44100., 48000., 96000.})
    {
        auto e = prepared(rate, 1, true);
        auto reference = prepared(rate, 1, false);
        require(e->getProcessingLatencySamples() == reference->getProcessingLatencySamples(),
                "VCF island added latency to existing VCA boundary");
        e->noteOn(72, 1); e->noteOn(79, .8f);
        std::array<float, 64> l {}, r {};
        double power = 0;
        for (int block = 0; block < 48; ++block)
        {
            if (block == 8)
            {
                auto p = YouKnowTestAccess::parameters(*e);
                p.cutoff = .3f; p.resonance = 1; p.pwmDepth = .9f;
                p.calibration = 1.7f; p.enableBa662TailMirrorNoise = true;
                e->setParameters(p);
            }
            if (block == 20) e->allNotesOff();
            if (block == 24) e->noteOn(91, 1);
            if (block == 30) require(YouKnowTestAccess::rebuild(*e, 2), "quality2 rebuild failed");
            if (block == 34) require(YouKnowTestAccess::rebuild(*e, 1), "quality1 rebuild failed");
            if (block == 38)
            {
                auto p = YouKnowTestAccess::parameters(*e); p.enableVoiceFilterRateIsland = false;
                e->setParameters(p);
            }
            if (block == 42)
            {
                auto p = YouKnowTestAccess::parameters(*e); p.enableVoiceFilterRateIsland = true;
                e->setParameters(p); e->noteOn(64, 1);
            }
            guardAllocation = true;
            e->process(l.data(), r.data(), 64);
            guardAllocation = false;
            for (float x : l) { require(std::isfinite(x), "nonfinite candidate output"); power += x*x; }
            YouKnowTestAccess::finite(*e);
        }
        require(power > 1e-7, "candidate engine produced no audio");
    }
}

void integratedControlsAndNoise()
{
    auto e = prepared(48000, 1, true);
    YouKnowTestAccess::coldNoiseAndControlChronology(*e);
    YouKnowTestAccess::unclippedInteriorControls(*e);
    for (double rate : {44100., 48000., 96000.})
        for (double frequency : {8000., 12000., 16000., 20000.}) {
            auto actual = prepared(rate, 1, true);
            const double response = YouKnowTestAccess::integratedResponse(*actual, frequency);
            const double analogue = -40*std::log10(1+std::pow(frequency/50000, 2));
            require(std::abs(response-analogue)<.09,
                "complete VCF/C59/VCA island missed analogue small-signal response");
        }
    require(allocations == 0, "candidate audio processing allocated");
}

}

int main()
{
    try
    {
        delayedControlParity(); intervalChronology(); unchangedDelay(); physicalCutoffResponse();
        engineRoutingAndLifecycle(); integratedControlsAndNoise();
        std::cout << "Voice-rate island transport, production response, engine controls/noise and lifecycle passed\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << error.what() << '\n'; return 1; }
}
