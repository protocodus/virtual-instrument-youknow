// processHoldingIdleCards() lets a host that skips silent callbacks keep the
// shared output path -- above all the engaged chorus and its BBD hiss --
// running while no voice sounds. These tests pin that its output is exactly
// process()'s while the voices are silent, that the hiss is really there,
// that the held cards keep their state and wake normally, and that holding
// never applies to a sounding instrument.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductHardwareRealism.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

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
    static auto filterState(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].filter.state;
    }
    static double ramp(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].dco.rampValue;
    }
};
}

namespace
{
using namespace youknow;
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

EngineParameters patch(ChorusMode chorus)
{
    EngineParameters p;
    ProductHardwareRealismProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.polyphony = 6;
    p.aging = .5f;
    p.calibration = 1;
    p.sawEnabled = true; p.pulseEnabled = true; p.subLevel = .3f;
    p.cutoff = .7f; p.resonance = .5f;
    p.attack = 0; p.decay = .5f; p.sustain = .8f; p.release = .2f;
    p.chorus = chorus;
    return p;
}

std::unique_ptr<YouKnowEngine> engine(ChorusMode chorus, CircuitRate rate)
{
    auto e = std::make_unique<YouKnowEngine>();
    ProductHardwareRealismProfile::configureBeforePrepare(*e);
    e->setInitialCircuitRate(rate);
    e->prepare(48000, 64, 1);
    e->setParameters(patch(chorus));
    return e;
}

// A chord, its release, and enough plain process() for every voice to retire.
void playAndRetire(YouKnowEngine& e)
{
    std::array<float, 64> left {}, right {};
    for (int note : {48, 55, 60, 64}) e.noteOn(note, .9f);
    for (int block = 0; block < 300; ++block) e.process(left.data(), right.data(), 64);
    for (int note : {48, 55, 60, 64}) e.noteOff(note);
    int blocks = 0;
    while (e.getActiveVoiceCount() > 0 && blocks < 3000)
    {
        e.process(left.data(), right.data(), 64);
        ++blocks;
    }
    require(e.getActiveVoiceCount() == 0, "the chord never retired");
    for (int block = 0; block < 225; ++block) e.process(left.data(), right.data(), 64);
}

bool same(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](float x, float y) {
        return std::bit_cast<std::uint32_t>(x) == std::bit_cast<std::uint32_t>(y);
    });
}

double rmsDb(const std::vector<float>& x)
{
    double sum = 0;
    for (float v : x) sum += static_cast<double>(v) * v;
    return 10 * std::log10(std::max(sum / static_cast<double>(x.size()), 1e-30));
}

void idleOutputIsProcessOutput()
{
    for (CircuitRate rate : {CircuitRate::Max, CircuitRate::Normal})
    {
        double floor = 0;
        for (ChorusMode chorus : {ChorusMode::Off, ChorusMode::One, ChorusMode::Two, ChorusMode::OneTwo})
        {
            auto running = engine(chorus, rate);
            auto held = engine(chorus, rate);
            playAndRetire(*running);
            playAndRetire(*held);
            const auto cardBefore = YouKnowTestAccess::filterState(*held, 0);
            const double rampBefore = YouKnowTestAccess::ramp(*held, 0);
            std::vector<float> a(96000), b(96000), c(96000), d(96000);
            guardAllocation = true;
            for (int at = 0; at < 96000; at += 64)
            {
                running->process(a.data() + at, c.data() + at, 64);
                held->processHoldingIdleCards(b.data() + at, d.data() + at, 64);
            }
            guardAllocation = false;
            require(same(a, b) && same(c, d),
                    "holding the idle cards changed the silent voices' output");
            require(YouKnowTestAccess::filterState(*held, 0) == cardBefore
                        && YouKnowTestAccess::ramp(*held, 0) == rampBefore,
                    "a held card advanced");
            require(YouKnowTestAccess::ramp(*running, 0) != rampBefore,
                    "the running reference card did not free-run");
            // The engine's own level (the Rack adds its +18 dB calibration):
            // the engaged chorus's hiss must stand well above the chorus-off
            // floor of the shared path, which is all that remains without it.
            const double level = rmsDb(b);
            std::cout << "  idle " << (rate == CircuitRate::Max ? "Max" : "Normal")
                      << " chorus mode " << static_cast<int>(chorus) << ": " << level << " dBFS\n";
            if (chorus == ChorusMode::Off) floor = level;
            else require(level > floor + 10, "the engaged chorus lost its idle hiss");
        }
    }
    require(allocations == 0, "holding the idle cards allocated");
    std::cout << "idle output: equals process() bit for bit; chorus hiss continues; held cards hold\n";
}

void heldCardsWake()
{
    // After a long hold the next chord starts like the free-running engine's:
    // the cards hold a self-consistent state, so no wake step appears.
    for (CircuitRate rate : {CircuitRate::Max, CircuitRate::Normal})
    {
        auto running = engine(ChorusMode::One, rate);
        auto held = engine(ChorusMode::One, rate);
        playAndRetire(*running);
        playAndRetire(*held);
        std::array<float, 64> left {}, right {};
        for (int block = 0; block < 1500; ++block)
        {
            running->process(left.data(), right.data(), 64);
            held->processHoldingIdleCards(left.data(), right.data(), 64);
        }
        std::vector<float> x(19200), y(19200), xr(19200), yr(19200);
        for (int note : {50, 57, 62}) { running->noteOn(note, .9f); held->noteOn(note, .9f); }
        for (int at = 0; at < 19200; at += 64)
        {
            running->process(x.data() + at, xr.data() + at, 64);
            held->processHoldingIdleCards(y.data() + at, yr.data() + at, 64);
        }
        for (float v : y) require(std::isfinite(v), "a woken held card rendered nonfinite audio");
        const double difference = rmsDb(y) - rmsDb(x);
        std::cout << "  wake " << (rate == CircuitRate::Max ? "Max" : "Normal")
                  << ": level " << difference << " dB against free-running cards\n";
        require(std::abs(difference) < .5, "a chord after the hold started at the wrong level");
        // With voices sounding, holding never applies: the next blocks match.
        std::vector<float> p(6400), q(6400), pr(6400), qr(6400);
        auto twin = engine(ChorusMode::Two, rate);
        auto other = engine(ChorusMode::Two, rate);
        for (auto* e : {twin.get(), other.get()}) e->noteOn(60, .9f);
        for (int at = 0; at < 6400; at += 64)
        {
            twin->process(p.data() + at, pr.data() + at, 64);
            other->processHoldingIdleCards(q.data() + at, qr.data() + at, 64);
        }
        require(same(p, q) && same(pr, qr), "holding applied to a sounding instrument");
    }
    std::cout << "idle output: held cards wake at the free-running level; a sounding instrument is never held\n";
}

void holdingCostsLess()
{
    // A diagnostic, not a bound: the held path skips all six cards.
    auto e = engine(ChorusMode::One, CircuitRate::Normal);
    playAndRetire(*e);
    std::array<float, 64> left {}, right {};
    const auto time = [&](bool hold) {
        const auto begin = std::chrono::steady_clock::now();
        for (int block = 0; block < 750; ++block)
            hold ? e->processHoldingIdleCards(left.data(), right.data(), 64)
                 : e->process(left.data(), right.data(), 64);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    };
    const double full = time(false), held = time(true);
    std::cout << "idle cost per audio second: process " << full << " s, held " << held << " s\n";
}
}

int main()
{
    try
    {
        idleOutputIsProcessOutput();
        heldCardsWake();
        holdingCostsLess();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "idle output tests passed\n";
    return 0;
}
