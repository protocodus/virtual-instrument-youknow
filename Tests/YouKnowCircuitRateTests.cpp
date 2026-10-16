// CircuitRate selects the local factor of the two numerical rate islands
// (YouKnowEngine.h). These tests pin its factor law, the unchanged latency,
// Max as the default and as an identity, the idle-deferred live change,
// Normal's closeness to Max through the product signal path, and Normal's
// idle-card rest: a tracked (not frozen) filter, host-partition independence
// and a wake that starts from that tracked state.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowChorusRateIsland.h"
#include "DSP/YouKnowProductHardwareRealism.h"
#include "DSP/YouKnowVoiceVcaAntialias.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numbers>
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
    static bool resting(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].islandResting;
    }
    static bool active(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].active;
    }
    static auto filterState(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].filter.state;
    }
    static double c59(const YouKnowEngine& e, int card)
    {
        return e.voices_[static_cast<std::size_t>(card)].vcaInputCoupling.state;
    }
    // What the host receives: the running FIR delays plus the pad that tops
    // them up to the one reported figure.
    static double delivered(const YouKnowEngine& e)
    {
        return e.runningLatencySamples(e.oversampling_) + e.latencyPadSamples_;
    }
    static int chorusFactor(const YouKnowEngine& e)
    {
        return e.chorusRateIslandKernel_.factor;
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

struct Event
{
    int frame;
    int note;
    bool on;
};

EngineParameters productParameters()
{
    EngineParameters p;
    ProductHardwareRealismProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.polyphony = 6;
    p.aging = .5f;
    p.calibration = 1;
    return p;
}

std::unique_ptr<YouKnowEngine> productEngine(double rate, int block,
    const EngineParameters& parameters, CircuitRate circuit, int quality = 1,
    bool initial = true)
{
    auto e = std::make_unique<YouKnowEngine>();
    ProductHardwareRealismProfile::configureBeforePrepare(*e);
    e->prepare(rate, block, 1);
    if (initial)
        e->setInitialCircuitRate(circuit);
    e->setParameters(parameters);
    e->setInitialOversamplingFactor(quality);
    return e;
}

// Renders `frames` stereo frames in host blocks drawn from `partition`
// (cycled), placing each event at the start of the block that contains it.
std::vector<float> render(YouKnowEngine& e, int frames, const std::vector<Event>& events,
                          const std::vector<int>& partition)
{
    std::vector<float> out(static_cast<std::size_t>(2 * frames));
    std::vector<float> left(4096), right(4096);
    std::size_t next = 0, part = 0;
    for (int written = 0; written < frames;)
    {
        const int n = std::min(partition[part++ % partition.size()], frames - written);
        while (next < events.size() && events[next].frame < written + n)
        {
            const auto& event = events[next++];
            if (event.on) e.noteOn(event.note, .85f); else e.noteOff(event.note);
        }
        e.process(left.data(), right.data(), n);
        for (int i = 0; i < n; ++i)
        {
            out[static_cast<std::size_t>(2 * (written + i))] = left[static_cast<std::size_t>(i)];
            out[static_cast<std::size_t>(2 * (written + i) + 1)] = right[static_cast<std::size_t>(i)];
        }
        written += n;
    }
    return out;
}

double power(const std::vector<float>& x, int first, int last)
{
    double sum = 0;
    for (int i = first; i < last; ++i)
        sum += static_cast<double>(x[static_cast<std::size_t>(i)])
             * x[static_cast<std::size_t>(i)];
    return sum / std::max(1, last - first);
}

void fft(std::vector<std::complex<double>>& x)
{
    const std::size_t n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1)
    {
        const auto root = std::polar(1., -2 * std::numbers::pi / static_cast<double>(length));
        for (std::size_t start = 0; start < n; start += length)
        {
            std::complex<double> w { 1., 0. };
            for (std::size_t k = 0; k < length / 2; ++k)
            {
                const auto u = x[start + k];
                const auto v = x[start + k + length / 2] * w;
                x[start + k] = u + v;
                x[start + k + length / 2] = u - v;
                w *= root;
            }
        }
    }
}

// Power of the mono sum in [lo, hi) Hz over the power-of-two frame span
// starting at `first`, from one Hann-windowed FFT.
double bandPower(const std::vector<float>& x, double rate, int first, int count,
                 double lo, double hi)
{
    std::vector<std::complex<double>> mono(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const double w = .5 - .5 * std::cos(2 * std::numbers::pi * i / count);
        mono[static_cast<std::size_t>(i)] = w * .5
            * (x[static_cast<std::size_t>(2 * (first + i))]
               + x[static_cast<std::size_t>(2 * (first + i) + 1)]);
    }
    fft(mono);
    double sum = 0;
    for (int bin = 1; bin < count / 2; ++bin)
    {
        const double frequency = bin * rate / count;
        if (frequency >= lo && frequency < hi)
            sum += std::norm(mono[static_cast<std::size_t>(bin)]);
    }
    return sum;
}

double db(double ratio) { return 10 * std::log10(std::max(ratio, 1e-300)); }

void factorLaw()
{
    struct Row { double rate; int max, normal; };
    for (const Row row : {Row{44100, 4, 2}, Row{48000, 4, 2}, Row{88200, 2, 1},
                          Row{96000, 2, 1}, Row{176400, 1, 1}, Row{192000, 1, 1}})
    {
        require(YouKnowEngine::voiceCircuitFactorFor(row.rate, CircuitRate::Max) == row.max,
                "Max voice factor is not the voice FIR's own grid");
        require(YouKnowEngine::voiceCircuitFactorFor(row.rate, CircuitRate::Normal) == row.normal,
                "Normal does not halve the voice factor down to the direct law");
        require(YouKnowEngine::voiceCircuitFactorFor(row.rate, CircuitRate::Max)
                    == VoiceVcaAntialias::factorForRate(row.rate),
                "Max changed the established voice factor law");
        require(row.rate * row.normal >= 88200 * .999 || row.normal == 1,
                "Normal fell below its 88.2 kHz local floor");
    }
    for (int global : {1, 2, 4})
        for (double host : {44100., 48000., 96000.})
        {
            const double parent = host * global;
            const int max = ChorusRateIsland::factorForRate(parent, global, false);
            const int normal = ChorusRateIsland::factorForRate(parent, global, true);
            require(normal == std::max(1, max / 2) || (max == 1 && normal == 1),
                    "Normal does not halve the chorus island factor");
        }
    std::cout << "circuit rate factor law: voice and chorus islands halve under Normal\n";
}

void latencyUnchanged()
{
    for (double rate : {44100., 48000., 96000.})
        for (int quality : {1, 2, 4})
        {
            const auto p = productParameters();
            auto max = productEngine(rate, 64, p, CircuitRate::Max, quality);
            auto normal = productEngine(rate, 64, p, CircuitRate::Normal, quality);
            require(max->getProcessingLatencySamples() == normal->getProcessingLatencySamples(),
                    "circuit rate changed the reported latency");
            const int factor = max->getOversamplingFactor();
            require(normal->getOversamplingFactor() == factor, "circuit rate changed Quality");
            // Where Normal reaches the direct law its voice FIR (and delay)
            // is gone, and the latency pad supplies the difference.
            require(std::abs(YouKnowTestAccess::delivered(*max)
                             - YouKnowTestAccess::delivered(*normal)) < 1e-9,
                    "circuit rate changed the delivered latency");
            require(normal->getVoiceCircuitFactor()
                        == YouKnowEngine::voiceCircuitFactorFor(rate * factor, CircuitRate::Normal),
                    "Normal engine runs the wrong voice factor");
            require(max->getVoiceCircuitFactor()
                        == YouKnowEngine::voiceCircuitFactorFor(rate * factor, CircuitRate::Max),
                    "Max engine runs the wrong voice factor");
        }
    // The delivered onset, not only the accounting: a short pluck must
    // cross-correlate best at zero lag between the two rates, on the grid
    // where both keep a voice FIR (1x) and where Normal drops it (2x).
    auto p = productParameters();
    p.attack = 0; p.decay = .25f; p.sustain = 0; p.release = .1f;
    p.cutoff = .8f; p.resonance = .1f; p.chorus = ChorusMode::Off;
    const int rate = 48000, frames = rate / 4;
    const std::vector<Event> events { {2400, 60, true}, {4800, 60, false} };
    for (int quality : {1, 2})
    {
        auto a = productEngine(rate, 64, p, CircuitRate::Max, quality);
        auto b = productEngine(rate, 64, p, CircuitRate::Normal, quality);
        const auto x = render(*a, frames, events, {64});
        const auto y = render(*b, frames, events, {64});
        int bestLag = 0; double best = -1;
        for (int lag = -64; lag <= 64; ++lag)
        {
            double sum = 0;
            for (int i = 2400; i < 2400 + 2400; ++i)
                sum += static_cast<double>(x[static_cast<std::size_t>(2 * i)])
                     * y[static_cast<std::size_t>(2 * (i + lag))];
            if (sum > best) { best = sum; bestLag = lag; }
        }
        require(bestLag == 0, "Normal moved the delivered onset");
    }
    std::cout << "circuit rate latency: reported, delivered and onset unchanged\n";
}

void maxIsDefaultAndIdentity()
{
    YouKnowEngine fresh;
    require(fresh.getCircuitRate() == CircuitRate::Max, "Max is not the default circuit rate");
    auto p = productParameters();
    p.chorus = ChorusMode::One;
    const std::vector<Event> events { {100, 48, true}, {100, 55, true}, {100, 64, true},
                                      {9000, 48, false}, {9000, 55, false}, {9000, 64, false} };
    auto untouched = productEngine(48000, 64, p, CircuitRate::Max, 1, false);
    auto selected = productEngine(48000, 64, p, CircuitRate::Max);
    require(untouched->getCircuitRate() == CircuitRate::Max, "default engine left Max");
    const auto a = render(*untouched, 14400, events, {64});
    const auto b = render(*selected, 14400, events, {64});
    require(std::equal(a.begin(), a.end(), b.begin(), [](float l, float r) {
                return std::bit_cast<std::uint32_t>(l) == std::bit_cast<std::uint32_t>(r);
            }), "selecting Max is not an identity");
    std::cout << "circuit rate default: Max, and selecting it is bit-identical\n";
}

void liveChangeWaitsForIdle()
{
    auto p = productParameters();
    p.release = .05f;
    auto e = productEngine(48000, 64, p, CircuitRate::Max);
    std::array<float, 64> left {}, right {};
    e->noteOn(60, .8f);
    for (int block = 0; block < 40; ++block) e->process(left.data(), right.data(), 64);
    require(!e->setCircuitRate(CircuitRate::Normal), "a sounding voice accepted a grid rebuild");
    require(!e->isOversamplingReady(), "a pending circuit rate reported ready");
    require(e->getCircuitRate() == CircuitRate::Max, "the circuit rate changed under a voice");
    for (int block = 0; block < 40; ++block)
    {
        e->process(left.data(), right.data(), 64);
        require(e->getVoiceCircuitFactor() == 4, "the voice grid changed under a held note");
    }
    e->noteOff(60);
    int blocks = 0;
    while (!e->isOversamplingReady() && blocks < 3000)
    {
        e->process(left.data(), right.data(), 64);
        for (float v : left) require(std::isfinite(v), "nonfinite audio during circuit change");
        ++blocks;
    }
    require(e->isOversamplingReady(), "the circuit rate change never applied once idle");
    require(e->getCircuitRate() == CircuitRate::Normal && e->getVoiceCircuitFactor() == 2
                && YouKnowTestAccess::chorusFactor(*e) == 2,
            "idle circuit change did not install the Normal grid");
    // Back to Max the same way, with no allocation in the audio callbacks.
    guardAllocation = true;
    (void) e->setCircuitRate(CircuitRate::Max);
    blocks = 0;
    while (!e->isOversamplingReady() && blocks < 3000)
    {
        e->process(left.data(), right.data(), 64);
        ++blocks;
    }
    e->noteOn(67, .8f);
    for (int block = 0; block < 100; ++block)
    {
        e->process(left.data(), right.data(), 64);
        for (float v : left) require(std::isfinite(v), "nonfinite audio after circuit change");
    }
    guardAllocation = false;
    require(allocations == 0, "a live circuit change allocated in the audio callback");
    require(e->getCircuitRate() == CircuitRate::Max && e->getVoiceCircuitFactor() == 4,
            "the Max grid did not return");
    std::cout << "circuit rate live change: deferred to idle, faded, allocation-free\n";
}

void normalStaysCloseToMax()
{
    // A bright chord through the product path: Normal's spectrum must stay
    // within a fraction of a decibel of Max in every band a listener hears,
    // and its level within a tenth of a decibel. The bounds sit above the
    // measured 2026-10-16 deltas (at most +0.33 dB in 14-20 kHz) with margin;
    // a pole cap or island wiring error moves these by whole decibels.
    auto p = productParameters();
    p.sawEnabled = true; p.pulseEnabled = true; p.pwmDepth = .35f;
    p.pwmSource = PwmSource::Manual; p.subLevel = .3f;
    p.cutoff = .96f; p.resonance = .35f; p.envDepth = 0; p.keyFollow = 0;
    p.attack = 0; p.decay = .6f; p.sustain = .9f; p.release = .35f;
    p.chorus = ChorusMode::Two;
    const double rate = 48000;
    const int frames = 48000 * 2;
    std::vector<Event> events;
    for (int note : {48, 55, 60, 64, 67, 72}) events.push_back({480, note, true});
    auto a = productEngine(rate, 64, p, CircuitRate::Max);
    auto b = productEngine(rate, 64, p, CircuitRate::Normal);
    const auto x = render(*a, frames, events, {64});
    const auto y = render(*b, frames, events, {64});
    const int first = 24000, count = 32768, last = first + count;
    const double level = db(power(y, 2 * first, 2 * last) / power(x, 2 * first, 2 * last));
    require(std::abs(level) < .1, "Normal changed the chord level");
    struct Band { double lo, hi, limit; };
    for (const Band band : {Band{20, 200, .25}, Band{200, 2000, .25}, Band{2000, 8000, .35},
                            Band{8000, 14000, .6}, Band{14000, 20000, .8}})
    {
        const double delta = db(bandPower(y, rate, first, count, band.lo, band.hi)
                                / bandPower(x, rate, first, count, band.lo, band.hi));
        std::cout << "  Normal - Max " << band.lo << "-" << band.hi << " Hz: " << delta << " dB\n";
        require(std::abs(delta) < band.limit, "Normal moved a band level beyond its bound");
    }
    std::cout << "circuit rate Normal: level and band levels close to Max\n";
}

void idleCardsRestTrackedAndWake()
{
    // Self-oscillation exposes the filter state a resting card hands to its
    // next note. Under Normal the idle cards must rest (no island), keep
    // their filter and C59 moving, render independently of the host
    // partition, and wake with the low band Max delivers.
    auto p = productParameters();
    p.sawEnabled = false; p.pulseEnabled = false; p.subLevel = 0; p.noiseLevel = 0;
    p.cutoff = .55f; p.resonance = 1; p.envDepth = 0; p.keyFollow = 1;
    p.attack = .05f; p.decay = 1; p.sustain = 1; p.release = .3f;
    p.chorus = ChorusMode::Off;
    const double rate = 48000;
    const std::vector<Event> events { {4800, 48, true}, {52800, 48, false},
                                      {67200, 60, true}, {120000, 60, false},
                                      {134400, 72, true}, {187200, 72, false} };
    const int frames = 196800;

    auto e = productEngine(rate, 64, p, CircuitRate::Normal);
    std::array<float, 64> left {}, right {};
    for (int block = 0; block < 75; ++block) e->process(left.data(), right.data(), 64);
    int resting = 0;
    for (int card = 0; card < YouKnowEngine::hardwareVoices; ++card)
        resting += YouKnowTestAccess::resting(*e, card) ? 1 : 0;
    require(resting == YouKnowEngine::hardwareVoices, "idle cards did not rest under Normal");
    const auto before = YouKnowTestAccess::filterState(*e, 0);
    const double c59 = YouKnowTestAccess::c59(*e, 0);
    for (int block = 0; block < 75; ++block) e->process(left.data(), right.data(), 64);
    require(YouKnowTestAccess::filterState(*e, 0) != before
                || YouKnowTestAccess::c59(*e, 0) != c59,
            "a resting card froze its filter instead of tracking it");
    for (const auto& x : YouKnowTestAccess::filterState(*e, 0))
        require(std::isfinite(x), "a resting card's filter state became nonfinite");

    auto max = productEngine(rate, 64, p, CircuitRate::Max);
    auto normal = productEngine(rate, 64, p, CircuitRate::Normal);

    const auto x = render(*max, frames, events, {64});
    const auto y = render(*normal, frames, events, {64});
    // Each wake: the first 85 ms after the later notes. A card that wakes
    // from a wrong (for instance frozen) state announces itself as a low-band
    // step under the new oscillation; the freeze rest this replaced measured
    // about -45 dB against the note there. Bound the low-band excess over Max
    // relative to the window's whole power, and the window level itself.
    for (int onset : {67200, 134400})
    {
        const int first = onset, last = onset + 4096;
        const double total = bandPower(x, rate, first, 4096, 0, rate / 2);
        const double excess = db(std::abs(bandPower(y, rate, first, 4096, 20, 400)
                                          - bandPower(x, rate, first, 4096, 20, 400)) / total);
        const double level = db(power(y, 2 * first, 2 * last) / power(x, 2 * first, 2 * last));
        std::cout << "  wake at " << onset << ": low-band excess " << excess
                  << " dB re window, level " << level << " dB\n";
        require(excess < -55 && std::abs(level) < .1, "a woken card started from the wrong state");
    }
    for (float v : y) require(std::isfinite(v), "Normal rest produced nonfinite audio");
    std::cout << "circuit rate idle cards: rest tracked, wake matches Max\n";
}

void restIsPartitionIndependent()
{
    // The rest advances whole internal frames, so the host's block partition
    // must not change a single bit, exactly as for the island itself.
    auto p = productParameters();
    p.cutoff = .6f; p.resonance = .7f; p.chorus = ChorusMode::One;
    // Frame 10880 starts a block in both partitions below (170 x 64, and
    // 192 + 32 x 334 for the cycled one), so both deliver the release there.
    const std::vector<Event> events { {0, 48, true}, {0, 60, true},
                                      {10880, 48, false}, {10880, 60, false} };
    auto a = productEngine(48000, 128, p, CircuitRate::Normal);
    auto b = productEngine(48000, 128, p, CircuitRate::Normal);
    // Two cards play while four rest, and then all six rest after the release.
    const auto x = render(*a, 24000, events, {64});
    const auto y = render(*b, 24000, events, {37, 1, 64, 90, 3, 128, 11});
    require(std::equal(x.begin(), x.end(), y.begin(), [](float l, float r) {
                return std::bit_cast<std::uint32_t>(l) == std::bit_cast<std::uint32_t>(r);
            }), "Normal's idle-card rest depends on the host block partition");
    std::cout << "circuit rate idle cards: rest independent of the host partition\n";
}
}

int main()
{
    try
    {
        factorLaw();
        latencyUnchanged();
        maxIsDefaultAndIdentity();
        liveChangeWaitsForIdle();
        normalStaysCloseToMax();
        idleCardsRestTrackedAndWake();
        restIsPartitionIndependent();
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    std::cout << "circuit rate tests passed\n";
    return 0;
}
