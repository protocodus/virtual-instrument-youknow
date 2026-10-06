#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

#include <chrono>
#include <complex>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace { bool allocationGuard {}; unsigned allocations {}; }
void* operator new(std::size_t n)
{
    if (allocationGuard) ++allocations;
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc {};
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace youknow {
struct YouKnowTestAccess
{
    static double rate(const YouKnowEngine& e) { return e.oversampledRate_; }
    static float modulate(YouKnowEngine& e, float signal)
    { return e.applyCommonVcaControlNoise(signal, e.activeParameters_); }
    static auto random(const YouKnowEngine& e) { return e.commonVcaControlRandom_; }
    static auto noise(const YouKnowEngine& e) { return e.commonVcaControlNoise_; }
    static double input(const YouKnowEngine& e) { return e.commonVcaOutputPole_.previousInput; }
    static double kelvin(const YouKnowEngine& e) { return e.jackBoardCelsius_+273.15; }
    static bool rebuildAtQuietBoundary(YouKnowEngine& e, int quality)
    {
        // Reproduce the idle safety fade's zero-gain boundary, then run the
        // actual public request/rebuild path without advancing a noise draw.
        e.oversamplingIdleSamples_ = e.oversamplingQuietSamples_;
        e.rateTransition_ = YouKnowEngine::RateTransition::FadingOut;
        e.rateTransitionGain_ = 0;
        return e.setOversamplingFactor(quality);
    }
    static void temperature(YouKnowEngine& e, float celsius)
    {
        e.jackBoardCelsius_ = celsius;
        e.jackBoardJohnsonScale_ = std::sqrt((celsius+273.15f)/298.15f);
    }
    static auto legacyStreams(const YouKnowEngine& e)
    {
        using Card = std::tuple<std::uint32_t, std::uint32_t, double, bool,
                                std::uint32_t, double, bool>;
        std::array<Card, YouKnowEngine::maxVoices> cards {};
        for (std::size_t i = 0; i < cards.size(); ++i) {
            const auto& v = e.voices_[i];
            cards[i] = {v.noiseState, v.filterShotRandom.state,
                v.filterShotRandom.spare, v.filterShotRandom.hasSpare,
                v.vcaShotRandom.state, v.vcaShotRandom.spare, v.vcaShotRandom.hasSpare};
        }
        return std::tuple {e.noiseState_, e.commonVcaNoiseState_,
            e.outputNoiseStateLeft_, e.outputNoiseStateRight_,
            e.outputWiperNoiseStateLeft_, e.outputWiperNoiseStateRight_, cards};
    }
};
}

namespace {
using namespace youknow;
using Complex = std::complex<long double>;
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

// Independent two-node complex KCL at the C7 hold and GC1 nodes. Stamp each
// drawn resistor's separate4kT/R Norton source, rather than using a reduced
// source impedance or the production FDT/source-weight formula.
std::pair<Complex, long double> nodal(double frequency, double kelvin)
{
    const Complex s {0, 2*std::numbers::pi_v<long double>*frequency};
    const Complex a = 1.L/2200+1.L/1500+s*10e-6L;
    const Complex b = -1.L/1500;
    const Complex d = 1.L/1500+1.L/47+1.L/15000;
    const Complex determinant = a*d-b*b;
    const Complex impedance = a/determinant;
    long double density = 0;
    const std::array<std::array<long double, 3>, 4> sources {{
        {{1, 0, 2200}}, {{-1, 1, 1500}}, {{0, 1, 47}}, {{0, 1, 15000}} }};
    for (const auto& source : sources) {
        const Complex voltage = (-b*source[0]+a*source[1])/determinant;
        density += std::norm(voltage)*4*1.380649e-23L*kelvin/source[2];
    }
    return {impedance, density};
}

void circuitOracle()
{
    double worst = 0;
    for (double frequency : {0., 1., 17.5, 100., 1000., 20000., 384000.})
        for (double kelvin : {273.15, 298.15, 313.15, 358.15}) {
            const auto [z, density] = nodal(frequency, kelvin);
            const Complex reduced = Complex {CommonVcaControlNoise::highResistance, 0}
                +static_cast<long double>(CommonVcaControlNoise::lowResistance-CommonVcaControlNoise::highResistance)
                  /Complex {1, 2*std::numbers::pi_v<long double>*frequency
                                *CommonVcaControlNoise::timeConstant};
            const double expected = 4*1.380649e-23*kelvin*static_cast<double>(z.real());
            worst = std::max(worst, std::abs(static_cast<double>(density)/expected-1));
            require(std::abs(reduced-z)/std::abs(z) < 2e-15L, "GC1 impedance misses independent KCL");
            require(std::abs(static_cast<double>(density)/expected-1) < 2e-15,
                    "four independent resistor sources disagree with FDT");
        }
    require(std::abs(YouKnowEngine::commonVcaHoldTimeConstantSeconds()
        /CommonVcaControlNoise::timeConstant-1) < 2e-7, "noise introduced a different C7 time constant");
    std::cout << "Independent complex GC1 KCL + four-source density relative error=" << worst << '\n';
}

void stochasticCircuit()
{
    constexpr double rate = 48000, kelvin = 313.15;
    const auto c = CommonVcaControlNoise::coefficients(rate);
    const double variance = 1.380649e-23*kelvin
        *(static_cast<double>(nodal(0, kelvin).first.real())
          -static_cast<double>(nodal(1e15, kelvin).first.real()))
        /CommonVcaControlNoise::timeConstant;
    CommonVcaControlNoise slow;
    OtaShotNoise::Random random {0x78324f15u};
    const double thermal = std::sqrt(kelvin/298.15);
    for (int i = 0; i < 8192; ++i) (void)slow.process(c, thermal, 0, random.next());
    double power = 0, correlation = 0, previous = slow.slowVolts;
    constexpr int count = 1<<22;
    for (int i = 0; i < count; ++i) {
        const double output = slow.process(c, thermal, 0, random.next());
        power += output*output/count;
        correlation += output*previous/count;
        previous = output;
    }
    std::cout << "C7 OU variance ratio=" << power/variance << '\n';
    require(std::abs(power/variance-1) < .05, "actual C7 OU variance is wrong");
    require(std::abs(correlation/power-c.decay) < .0004, "C7 covariance decay is wrong");
    // Changing rates or thermal innovation must retain the existing voltage.
    const double held = slow.slowVolts;
    const auto other = CommonVcaControlNoise::coefficients(192000);
    require(slow.process(other, 2, 0, 0) == held*other.decay, "rate/T change rescaled stored noise charge");
    slow.reset(); require(slow.slowVolts == 0, "hard reset retained C7 noise voltage");
    std::cout << "Actual slow-source variance ratio=" << power/variance
              << "; sampled covariance, thermal/rate memory passed\n";
}

std::unique_ptr<YouKnowEngine> prepared(bool enabled, double rate=48000, int quality=1,
                                      float character=1)
{
    auto e = std::make_unique<YouKnowEngine>();
    EngineParameters p;
    p.enableCommonVcaControlNoise = enabled;
    p.enableCommonVcaOutputPole = true;
    p.enableCommonVcaNoise = false;
    p.enableCardJohnsonFloor = false;
    p.chorus = ChorusMode::Off; p.chorusNoise = p.noiseLevel = 0;
    p.calibration = character; p.attack = p.release = 0; p.sustain = 1;
    e->setParameters(p); e->prepare(rate, 128, quality);
    return e;
}

void actualAmplitudeModulation()
{
    double worst = 0;
    for (double host : {8000., 44100., 48000., 96000., 192000.})
        for (int quality : {1, 2, 4})
            for (float celsius : {25.f, 40.f}) {
                auto e = prepared(true, host, quality);
                YouKnowTestAccess::temperature(*e, celsius);
                const double fs = YouKnowTestAccess::rate(*e), kelvin = YouKnowTestAccess::kelvin(*e);
                const double rhigh = static_cast<double>(nodal(1e15, kelvin).first.real());
                const double rlow = static_cast<double>(nodal(0, kelvin).first.real());
                const double voltageVariance = 2*1.380649e-23*kelvin*rhigh*fs
                    +1.380649e-23*kelvin*(rlow-rhigh)/CommonVcaControlNoise::timeConstant;
                const double slope = std::log(10.)/(20*.0059*(kelvin/298.15));
                for (float input : {-8.f, 1.f, 8.f}) {
                    double power = 0;
                    constexpr int count = 1<<17;
                    for (int i = 0; i < count; ++i) {
                        const double noise = YouKnowTestAccess::modulate(*e, input)-input;
                        power += noise*noise/count;
                    }
                    const double expected = input*input*slope*slope*voltageVariance;
                    worst = std::max(worst, std::abs(power/expected-1));
                    require(std::abs(power/expected-1) < .025, "actual gain modulation density/units/rate is wrong");
                }
                const auto before = YouKnowTestAccess::random(*e).state;
                for (int i = 0; i < 20; ++i)
                    require(YouKnowTestAccess::modulate(*e, 0) == 0, "GC1 modulation became an additive idle floor");
                require(before != YouKnowTestAccess::random(*e).state, "quiet shared board stopped its new stream");
            }
    // One independent draw/history step confirms the NEC sign and T scaling,
    // not merely power: positive GC1 voltage reduces gain.
    auto e = prepared(true);
    auto random = YouKnowTestAccess::random(*e);
    const double white = random.next(), slow = random.next();
    const double rhi = static_cast<double>(nodal(1e15, 298.15).first.real());
    const double rlo = static_cast<double>(nodal(0, 298.15).first.real());
    const double tau = (2200*(1500+47.*15000/(47+15000))/(2200+1500+47.*15000/(47+15000)))*1e-5;
    const double voltage = white*std::sqrt(2*1.380649e-23*298.15*rhi*48000)
        +slow*std::sqrt(1.380649e-23*298.15*(rlo-rhi)/tau*-std::expm1(-2/(48000*tau)));
    const float expected = static_cast<float>(8*(1-voltage*std::log(10.)/(20*.0059)));
    require(YouKnowTestAccess::modulate(*e, 8) == expected, "GC1 sign/current-time T coordinate changed");
    std::cout << "Actual engine AM variance max relative=" << worst
              << "; all rates/quality/T, signal sign and zero-input gating passed\n";
}

void sharedBusWiring()
{
    auto off = prepared(false), on = prepared(true);
    off->noteOn(60, 1); on->noteOn(60, 1);
    auto random = YouKnowTestAccess::random(*on);
    double slowState = 0, largest = 0;
    std::array<float, 1> left {}, right {};
    const double rhi = static_cast<double>(nodal(1e15, 298.15).first.real());
    const double rlo = static_cast<double>(nodal(0, 298.15).first.real());
    const double tau = (2200*(1500+47.*15000/(47+15000))/(2200+1500+47.*15000/(47+15000)))*1e-5;
    const double decay = std::exp(-1/(48000*tau));
    for (int i = 0; i < 3000; ++i) {
        off->process(left.data(), right.data(), 1);
        on->process(left.data(), right.data(), 1);
        const double kelvin = YouKnowTestAccess::kelvin(*on);
        const double white = random.next(), slow = random.next();
        slowState = decay*slowState+slow*std::sqrt(1.380649e-23*kelvin
            *(rlo-rhi)/tau*-std::expm1(-2/(48000*tau)));
        const double voltage = white*std::sqrt(2*1.380649e-23*kelvin*rhi*48000)+slowState;
        const float expected = static_cast<float>(YouKnowTestAccess::input(*off)
            *(1-voltage*std::log(10.)/(20*.0059*(kelvin/298.15))));
        largest = std::max(largest, std::abs(YouKnowTestAccess::input(*on)-expected));
        require(std::abs(YouKnowTestAccess::input(*on)-expected) < 2e-7,
                "actual shared bus did not apply GC1 noise before C5/split");
    }
    require(random.state == YouKnowTestAccess::random(*on).state,
            "shared bus innovation chronology differs from physical intervals");
    std::cout << "Actual full-engine pre-C5 wiring witness max=" << largest << " V-model units\n";
}

struct Render
{
    std::vector<float> audio;
    decltype(YouKnowTestAccess::legacyStreams(std::declval<YouKnowEngine&>())) legacy;
    std::uint32_t controlState {};
    double slow {};
};
Render render(bool enabled, float character, double rate, int quality, int block)
{
    auto e = prepared(enabled, rate, quality, character);
    e->noteOn(60, 1);
    Render r; r.audio.resize(2048);
    std::vector<float> right(r.audio.size());
    int at = 0;
    for (int scene = 0; scene < 4; ++scene) {
        if (scene == 1) e->allNotesOff();
        if (scene == 2) e->noteOn(72, 1);
        if (scene == 3) e->setOversamplingFactor(quality == 1 ? 4 : 1);
        const int end = (scene+1)*512;
        while (at < end) {
            const int n = std::min(block, end-at);
            allocationGuard = true;
            e->process(r.audio.data()+at, right.data()+at, n);
            allocationGuard = false;
            at += n;
        }
    }
    for (float x : r.audio) require(std::isfinite(x), "GC1 lifecycle produced nonfinite audio");
    r.legacy = YouKnowTestAccess::legacyStreams(*e);
    r.controlState = YouKnowTestAccess::random(*e).state;
    r.slow = YouKnowTestAccess::noise(*e).slowVolts;
    return r;
}
void lifecycle()
{
    require(!EngineParameters {}.enableCommonVcaControlNoise, "raw GC1 flag default changed");
    EngineParameters product; ProductFidelityProfile::applyTo(product);
    require(product.enableCommonVcaControlNoise, "product omitted GC1 resistor AM");
    for (double rate : {8000., 44100., 48000., 96000., 192000.})
        for (int quality : {1, 2, 4}) {
            const auto offZero = render(false, 0, rate, quality, 128);
            const auto onZero = render(true, 0, rate, quality, 128);
            require(offZero.audio == onZero.audio && offZero.legacy == onZero.legacy,
                    "Character0 GC1 selection changed reference audio or old streams");
            const auto off = render(false, 1, rate, quality, 137);
            const auto on = render(true, 1, rate, quality, 137);
            const auto singles = render(true, 1, rate, quality, 1);
            require(on.audio == singles.audio && on.controlState == singles.controlState
                && on.slow == singles.slow, "GC1 block partition changed state/audio");
            require(off.legacy == on.legacy, "GC1 noise consumed existing source RNG");
            require(off.audio != on.audio, "GC1 modulation did not reach LINE audio");
            require(off.controlState == 0x6412b9a7u && off.slow == 0,
                    "disabled raw reference advanced the new comparison stream");
        }
    auto used = prepared(true), fresh = prepared(true);
    for (int i = 0; i < 30; ++i) (void)YouKnowTestAccess::modulate(*used, 1);
    const double held = YouKnowTestAccess::noise(*used).slowVolts;
    auto random = YouKnowTestAccess::random(*used);
    require(held != 0 && YouKnowTestAccess::rebuildAtQuietBoundary(*used, 4)
        && YouKnowTestAccess::rate(*used) == 192000,
        "GC1 quality witness did not apply an actual rate rebuild");
    require(YouKnowTestAccess::noise(*used).slowVolts == held
        && YouKnowTestAccess::random(*used).state == random.state,
        "applied quality rebuild reset physical C7 charge or consumed a draw");
    const double kelvin = YouKnowTestAccess::kelvin(*used);
    const double tau = (2200*(1500+47.*15000/(47+15000))/(2200+1500+47.*15000/(47+15000)))*1e-5;
    const double rhi = static_cast<double>(nodal(1e15, kelvin).first.real());
    const double rlo = static_cast<double>(nodal(0, kelvin).first.real());
    const double white = random.next(), slow = random.next();
    const double voltage = white*std::sqrt(2*1.380649e-23*kelvin*rhi*192000)
        +held*std::exp(-1/(192000*tau))
        +slow*std::sqrt(1.380649e-23*kelvin*(rlo-rhi)/tau*-std::expm1(-2/(192000*tau)));
    const float expected = static_cast<float>(8*(1-voltage*std::log(10.)/(20*.0059*(kelvin/298.15))));
    require(YouKnowTestAccess::modulate(*used, 8) == expected,
        "applied quality rebuild retained old-rate GC1 innovation coefficients");
    const double continued = YouKnowTestAccess::noise(*used).slowVolts;
    const auto continuedRandom = YouKnowTestAccess::random(*used);
    require(YouKnowTestAccess::rebuildAtQuietBoundary(*used, 1)
        && YouKnowTestAccess::rate(*used) == 48000
        && YouKnowTestAccess::noise(*used).slowVolts == continued
        && YouKnowTestAccess::random(*used).state == continuedRandom.state,
        "return quality rebuild discarded physical C7 noise memory");
    used->reset();
    require(YouKnowTestAccess::noise(*used).slowVolts == 0
        && YouKnowTestAccess::random(*used).state == YouKnowTestAccess::random(*fresh).state,
        "reset failed to restore GC1 capacitor/noise state");
    require(allocations == 0, "shared GC1 noise allocated during processing");
    std::cout << "Character0/reference exact; old RNG unchanged; block/reset/idle/quality/noalloc passed\n";
}

double cost(bool enabled, int voices, int quality)
{
    auto e = std::make_unique<YouKnowEngine>();
    EngineParameters p; ProductFidelityProfile::applyTo(p);
    p.enableCommonVcaControlNoise = enabled; p.chorus = ChorusMode::Off;
    p.chorusNoise = p.noiseLevel = 0; p.polyphony = voices;
    p.attack = 0; p.sustain = 1; p.cutoff = .55f; p.resonance = .4f;
    e->setParameters(p); e->prepare(48000, 128, quality);
    for (int i = 0; i < voices; ++i) e->noteOn(48+2*i, 1);
    std::array<float, 128> left {}, right {};
    for (int i = 0; i < 30; ++i) e->process(left.data(), right.data(), 128);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 150; ++i) e->process(left.data(), right.data(), 128);
    return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
}
void costs()
{
    for (int voices : {1, 6}) for (int quality : {1, 4}) {
        double off = 0, on = 0;
        for (int repeat = 0; repeat < 3; ++repeat) {
            if (repeat%2 == 0) { off += cost(false, voices, quality); on += cost(true, voices, quality); }
            else { on += cost(true, voices, quality); off += cost(false, voices, quality); }
        }
        std::cout << "GC1 CPU " << voices << " voices quality" << quality << ": "
                  << off/3 << " -> " << on/3 << " s/.4s (ratio " << on/off << ")\n";
    }
}
}

int main()
{
    std::cout << std::unitbuf;
    try {
        circuitOracle(); stochasticCircuit(); actualAmplitudeModulation();
        sharedBusWiring(); lifecycle(); costs();
    } catch (const std::exception& e) {
        allocationGuard = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
