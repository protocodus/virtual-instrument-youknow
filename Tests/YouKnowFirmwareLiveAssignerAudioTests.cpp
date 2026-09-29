// Bounded live RXB input is equivalent to the existing immutable comparison
// input when given identical byte-ready coordinates. This is an input transport
// contract, not a new physical UART phase or host-MIDI latency model. The
// continuous A-5 portion below independently pins original-ROM byte coordinates
// and decodes only module pin levels using an explicit frame-end RX convention.
// Instruction/bus/capacitor behavior is also covered by retained serial suites.
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include "../Source/DSP/YouKnowFirmwareAssignerAudioBridge.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <tuple>
#include <vector>

static bool countingAllocations = false;
static std::size_t allocations = 0;
void *operator new(std::size_t n) {
    if (countingAllocations)
        ++allocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
using namespace youknow;
using Engine = YouKnowEngine;
using S = FirmwareSerialTrace;
std::uint64_t assertions = 0;
void check(bool condition, const char *message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}
namespace youknow {
struct YouKnowTestAccess {
    static auto queue(const Engine &e) {
        return std::span<const S::ByteReady>(e.firmwareSerialStream_)
            .subspan(e.firmwareSerialStreamHead_, e.firmwareSerialStreamCount_);
    }
    static unsigned activeCards(const Engine &e) {
        unsigned mask = 0;
        for (unsigned i = 0; i < 6; ++i)
            if (e.voices_[i].active)
                mask |= 1u << i;
        return mask;
    }
    static auto head(const Engine &e) { return e.firmwareSerialStreamHead_; }
    static auto physical(const Engine &e) {
        std::array<double, 6 * 24> values{};
        std::size_t i = 0;
        for (unsigned card = 0; card < 6; ++card) {
            const auto &v = e.voices_[card];
            const auto &d = v.dco;
            for (double x :
                 {double(d.divider), double(d.pendingDivider), double(d.pendingDividerValid),
                  double(d.pitState), double(d.pitOutHigh), d.pitClocksToEvent, d.rampValue,
                  d.rampSlopePerSecond, d.resetSecondsRemaining, double(v.dcoCv),
                  double(v.dcoCvTarget), double(v.cutoffCounts), double(v.cutoffCountsTarget),
                  double(v.vcaControl), double(v.vcaControlTarget), double(v.active),
                  double(v.freewheeling), double(v.envelope.level)})
                values[i++] = x;
            for (double x : v.filter.state)
                values[i++] = x;
        }
        return values;
    }
};
} // namespace youknow
using X = YouKnowTestAccess;
bool sameState(const S::State &a, const S::State &b) {
    const auto &x = a.registers;
    const auto &y = b.registers;
    return a.control.ram == b.control.ram && a.control.adcComplete == b.control.adcComplete &&
           a.now == b.now && a.rxb == b.rxb && a.ordinal == b.ordinal &&
           a.adc.elapsedStates == b.adc.elapsedStates && a.adc.conversion == b.adc.conversion &&
           a.adc.channel == b.adc.channel && a.adc.anm == b.adc.anm &&
           a.adc.request == b.adc.request &&
           a.adc.statesUntilConversion == b.adc.statesUntilConversion &&
           a.serialInterrupts == b.serialInterrupts && a.adcInterrupts == b.adcInterrupts &&
           a.passes == b.passes && a.fsr == b.fsr && a.rxbFull == b.rxbFull &&
           a.pending.start == b.pending.start && a.pending.returnPc == b.pending.returnPc &&
           a.pending.savedPsw == b.pending.savedPsw &&
           a.pending.interruptVector == b.pending.interruptVector &&
           a.pending.sampledSkip == b.pending.sampledSkip &&
           a.pending.skipped == b.pending.skipped && a.pending.remaining == b.pending.remaining &&
           a.pending.kind == b.pending.kind && a.pending.address == b.pending.address &&
           a.pending.accessDone == b.pending.accessDone &&
           a.pending.sampledValue == b.pending.sampledValue &&
           a.needsArbitration == b.needsArbitration &&
           a.completedInstruction == b.completedInstruction &&
           a.adc.interruptsEnabled == b.adc.interruptsEnabled &&
           a.adc.eiDeferred == b.adc.eiDeferred && a.adc.mkh == b.adc.mkh &&
           a.opaquePsw == b.opaquePsw &&
           std::tie(x.a, x.b, x.c, x.d, x.e, x.h, x.l, x.ea, x.v, x.alternateA, x.alternateB,
                    x.alternateC, x.alternateD, x.alternateE, x.alternateH, x.alternateL,
                    x.alternateEa, x.alternateV, x.pc, x.sp, x.carry, x.skip, x.pa, x.pb, x.portc,
                    x.portf) == std::tie(y.a, y.b, y.c, y.d, y.e, y.h, y.l, y.ea, y.v, y.alternateA,
                                         y.alternateB, y.alternateC, y.alternateD, y.alternateE,
                                         y.alternateH, y.alternateL, y.alternateEa, y.alternateV,
                                         y.pc, y.sp, y.carry, y.skip, y.pa, y.pb, y.portc, y.portf);
}

EngineParameters parameters() {
    EngineParameters p;
    p.calibration = p.aging = p.chorusNoise = p.velocityDepth = 0;
    p.chorus = ChorusMode::One;
    p.attack = 0;
    p.decay = .05f;
    p.sustain = .6f;
    p.release = .02f;
    p.cutoff = .6f;
    ProductFidelityProfile::applyTo(p);
    return p;
}
std::unique_ptr<Engine> create(double rate, int factor, bool streaming,
                               std::span<const S::ByteReady> bytes = {}) {
    auto engine = std::make_unique<Engine>();
    ProductFidelityProfile::configureBeforePrepare(*engine);
    engine->setParameters(parameters());
    Engine::FirmwareSerialReplayConfiguration configuration;
    configuration.streaming = streaming;
    configuration.schedule = bytes;
    check(engine->configureFirmwareSerialReplay(configuration), "valid replay configuration");
    engine->prepare(rate, 128, factor);
    return engine;
}
bool sameBytes(std::span<const S::ByteReady> a, std::span<const S::ByteReady> b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](auto x, auto y) {
               return x.states == y.states && x.value == y.value;
           });
}
void equalEngines(const Engine &a, const Engine &b) {
    check(a.firmwareSerialStatus() == S::Status::ReachedTarget &&
              b.firmwareSerialStatus() == S::Status::ReachedTarget,
          "both replay modes remain valid");
    check(sameState(a.firmwareSerialState(), b.firmwareSerialState()),
          "CPU, RAM, ADC and pending work exact");
    check(a.firmwareSerialAudioStates() == b.firmwareSerialAudioStates(),
          "fractional CPU time exact");
    check(X::physical(a) == X::physical(b), "physical PIT, ramps, CV, ENV and VCF states exact");
    const auto &x = a.firmwareSerialEvents();
    const auto &y = b.firmwareSerialEvents();
    check(x.count == y.count, "last interval event counts exact");
    for (std::size_t i = 0; i < x.count; ++i) {
        const auto &c = x.entries[i];
        const auto &d = y.entries[i];
        check(std::tie(c.kind, c.states, c.pc, c.address, c.value, c.card, c.phaseBits, c.pa, c.pb,
                       c.portc) == std::tie(d.kind, d.states, d.pc, d.address, d.value, d.card,
                                            d.phaseBits, d.pa, d.pb, d.portc),
              "last interval physical bus and RAM events exact");
    }
}
void pairedBlock(Engine &a, Engine &b, unsigned count, std::vector<float> *retained = nullptr) {
    std::array<float, 128> al{}, ar{}, bl{}, br{};
    countingAllocations = true;
    a.process(al.data(), ar.data(), int(count));
    b.process(bl.data(), br.data(), int(count));
    countingAllocations = false;
    check(allocations == 0, "actual live and immutable process do not allocate");
    for (unsigned i = 0; i < count; ++i) {
        check(al[i] == bl[i] && ar[i] == br[i], "actual stereo audio is bit-identical");
        check(std::isfinite(al[i]) && std::isfinite(ar[i]), "actual stereo audio finite");
        if (retained) {
            retained->push_back(al[i]);
            retained->push_back(ar[i]);
        }
    }
    equalEngines(a, b);
}
std::vector<S::ByteReady> score() {
    // State zero intentionally exercises the only permitted already-current
    // coordinate. Remaining bytes are separated enough to avoid RX overrun.
    return {{0, 0x88},      {1280, 48},     {8000, 0x89},   {9280, 55},    {20000, 0x86},
            {40000, 0x95},  {41280, 25},    {60000, 0x8e},  {61280, 0x54}, {80000, 0x80},
            {100000, 0x81}, {140000, 0x87}, {160000, 0x88}, {161280, 60},  {200000, 0x80}};
}
void matrix() {
    const auto bytes = score();
    for (unsigned rate : {44100u, 48000u, 96000u})
        for (int factor : {1, 4}) {
            std::vector<float> canonical;
            for (unsigned chunk : {1u, 83u}) {
                auto fixed = create(rate, factor, false, bytes);
                auto live = create(rate, factor, true);
                const unsigned frames = unsigned(std::ceil(.06 * rate));
                std::vector<float> audio;
                audio.reserve(frames * 2);
                std::size_t cursor = 0;
                for (unsigned frame = 0; frame < frames;) {
                    const unsigned n = std::min(chunk, frames - frame);
                    const auto limit = std::uint64_t(std::ceil((frame + n) * 4000000.L / rate));
                    auto end = cursor;
                    while (end < bytes.size() && bytes[end].states <= limit)
                        ++end;
                    countingAllocations = true;
                    const bool accepted = live->appendFirmwareSerialBytes(
                        std::span<const S::ByteReady>(bytes).subspan(cursor, end - cursor));
                    countingAllocations = false;
                    check(accepted && allocations == 0,
                          "just-in-time append accepted without allocation");
                    cursor = end;
                    pairedBlock(*fixed, *live, n, &audio);
                    frame += n;
                }
                check(cursor == bytes.size() && live->pendingFirmwareSerialBytes() == 0,
                      "every delivered byte consumed");
                check(std::any_of(audio.begin(), audio.end(),
                                  [](float x) { return std::abs(x) > 1e-7f; }),
                      "comparison includes nonzero actual voice audio");
                if (chunk == 1)
                    canonical = audio;
                else
                    check(audio == canonical, "block partition leaves actual audio bit-identical");
            }
        }
}
void rejectWithoutMutation(Engine &engine, std::span<const S::ByteReady> bytes) {
    const auto pending = X::queue(engine);
    std::array<S::ByteReady, Engine::firmwareSerialStreamingCapacity> copy{};
    std::copy(pending.begin(), pending.end(), copy.begin());
    const auto count = pending.size();
    const auto head = X::head(engine);
    const auto state = engine.firmwareSerialState();
    const auto physical = X::physical(engine);
    countingAllocations = true;
    const bool accepted = engine.appendFirmwareSerialBytes(bytes);
    countingAllocations = false;
    check(!accepted && allocations == 0, "invalid append rejected without allocation");
    check(sameBytes(X::queue(engine), std::span(copy).first(count)) && X::head(engine) == head,
          "rejected batch leaves queue and compaction position unchanged");
    check(sameState(state, engine.firmwareSerialState()) && physical == X::physical(engine),
          "rejected batch leaves CPU and actual circuitry unchanged");
}
void queueContracts() {
    auto unprepared = std::make_unique<Engine>();
    const std::array<S::ByteReady, 1> zero{{{0, 0x88}}};
    check(!unprepared->appendFirmwareSerialBytes(zero), "unprepared append rejected");
    Engine::FirmwareSerialReplayConfiguration invalid;
    invalid.streaming = true;
    invalid.schedule = zero;
    check(!unprepared->configureFirmwareSerialReplay(invalid),
          "two simultaneous input sources rejected");
    auto immutable = create(48000, 1, false);
    rejectWithoutMutation(*immutable, zero);
    auto live = create(48000, 1, true);
    check(live->appendFirmwareSerialBytes({}), "empty append valid in streaming mode");
    check(live->appendFirmwareSerialBytes(zero), "time zero accepted before first render");
    std::array<S::ByteReady, 2> tied{{{1000, 0x95}, {1000, 20}}};
    check(live->appendFirmwareSerialBytes(std::span(tied).first(1)) &&
              live->appendFirmwareSerialBytes(std::span(tied).last(1)),
          "equal timestamp batches preserve arrival order");
    check(X::queue(*live)[1].value == 0x95 && X::queue(*live)[2].value == 20,
          "same-coordinate bytes retain order rather than being sorted by value");
    tied[0].value = 0;
    tied[1].value = 0;
    check(X::queue(*live)[1].value == 0x95 && X::queue(*live)[2].value == 20,
          "append owns copied bytes rather than caller memory");
    const std::array<S::ByteReady, 2> unsorted{{{3000, 0x95}, {2000, 20}}};
    rejectWithoutMutation(*live, unsorted);
    const std::array<S::ByteReady, 1> beforeTail{{{999, 0x88}}};
    rejectWithoutMutation(*live, beforeTail);
    live->reset();
    check(live->pendingFirmwareSerialBytes() == 0 && live->firmwareSerialState().now == 0,
          "reset clears queue and CPU origin");
    check(live->appendFirmwareSerialBytes(zero), "reset restores time-zero append allowance");
    float l, r;
    live->process(&l, &r, 1);
    const auto now = live->firmwareSerialState().now;
    const std::array<S::ByteReady, 1> equalNow{{{now, 0x88}}}, past{{{now - 1, 0x88}}};
    rejectWithoutMutation(*live, equalNow);
    rejectWithoutMutation(*live, past);
    // Valid append guard is status-sensitive: an actual receive overrun is
    // retained, not silently repaired by accepting more bytes.
    live->reset();
    const std::array<S::ByteReady, 2> overrun{{{0, 0x88}, {0, 48}}};
    check(live->appendFirmwareSerialBytes(overrun),
          "input transport retains valid equal-time order");
    live->process(&l, &r, 1);
    check(live->firmwareSerialStatus() == S::Status::ReceiveOverrun,
          "receiver exposes actual same-time overrun");
    rejectWithoutMutation(*live, {});
}
void compaction() {
    std::vector<S::ByteReady> bytes;
    for (unsigned i = 0; i < 2048; ++i)
        bytes.push_back({1000 + 1280ULL * i, std::uint8_t(i % 2 ? 30 : 0x95)});
    auto fixed = create(48000, 1, false, bytes), live = create(48000, 1, true);
    check(live->appendFirmwareSerialBytes(std::span(bytes).first(1024)),
          "exact queue capacity accepted");
    rejectWithoutMutation(*live, std::span(bytes).subspan(1024, 1));
    unsigned frames = 0;
    while (live->pendingFirmwareSerialBytes() > 424) {
        pairedBlock(*fixed, *live, 83);
        frames += 83;
    }
    const auto consumed = 1024 - live->pendingFirmwareSerialBytes();
    check(consumed >= 600 && X::head(*live) > 0, "consumption leaves a nonempty displaced queue");
    // Reject an invalid batch that would require compaction: it must not even
    // move the surviving queue before discovering the later timestamp fault.
    const auto refill = std::span(bytes).subspan(1024, consumed);
    std::vector<S::ByteReady> invalid(refill.begin(), refill.end());
    invalid.back().states = 0;
    rejectWithoutMutation(*live, invalid);
    countingAllocations = true;
    const bool appended = live->appendFirmwareSerialBytes(std::span(bytes).subspan(1024, consumed));
    countingAllocations = false;
    check(appended && allocations == 0 && X::head(*live) == 0,
          "compaction and refill stay allocation-free");
    check(live->pendingFirmwareSerialBytes() == 1024 &&
              sameBytes(X::queue(*live), std::span(bytes).subspan(consumed, 1024)),
          "compaction retains exact surviving and appended sequence");
    const unsigned total =
        unsigned(std::ceil((bytes[1023 + consumed].states + 100) * 48000. / 4000000.));
    while (frames < total) {
        const unsigned n = std::min(83u, total - frames);
        pairedBlock(*fixed, *live, n);
        frames += n;
    }
    check(live->pendingFirmwareSerialBytes() == 0, "compacted queue consumed fully");
}
void lifecycle() {
    const auto bytes = score();
    auto fixed = create(48000, 1, false, bytes), live = create(48000, 1, true);
    check(live->appendFirmwareSerialBytes(bytes), "initial live schedule accepted");
    pairedBlock(*fixed, *live, 83);
    fixed->reset();
    live->reset();
    check(live->pendingFirmwareSerialBytes() == 0, "reset discards pending future input");
    check(live->appendFirmwareSerialBytes(bytes), "reset can receive same performance anew");
    for (unsigned i = 0; i < 40; ++i)
        pairedBlock(*fixed, *live, 83);
    const std::array<S::ByteReady, 2> future{{{200000, 0x88}, {201280, 61}}};
    fixed = create(48000, 4, false, future);
    live = create(48000, 4, true);
    check(live->appendFirmwareSerialBytes(future),
          "future queue accepted before quality transition");
    pairedBlock(*fixed, *live, 64);
    const auto state = live->firmwareSerialState();
    const auto physical = X::physical(*live);
    check(!fixed->setOversamplingFactor(1) && !live->setOversamplingFactor(1),
          "rate change follows deferred quality policy");
    check(sameState(state, live->firmwareSerialState()) && physical == X::physical(*live) &&
              sameBytes(X::queue(*live), future),
          "request retains CPU, capacitors and queue");
    unsigned processed = 64;
    while (live->getOversamplingFactor() != 1 && processed < 2000) {
        pairedBlock(*fixed, *live, 1);
        ++processed;
    }
    check(live->getOversamplingFactor() == 1 && live->pendingFirmwareSerialBytes() == 2,
          "actual rate transition retains both future bytes");
    for (unsigned i = 0; i < 60; ++i)
        pairedBlock(*fixed, *live, 83);
    check(live->pendingFirmwareSerialBytes() == 0, "retained bytes execute after rate transition");
    live->reset();
    check(live->appendFirmwareSerialBytes(future), "queue refilled before profile reset");
    live->selectConverterTimingProfile(Engine::ConverterTimingProfile::NormalizedServiceChart);
    live->reset();
    check(live->pendingFirmwareSerialBytes() == 0,
          "reset into normal profile also clears live queue");
    rejectWithoutMutation(*live, future);
    live->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareSerialReplay);
    live->reset();
    check(live->pendingFirmwareSerialBytes() == 0 && live->appendFirmwareSerialBytes(future),
          "return to serial after reset starts with a fresh queue");
}

namespace liveComposition {
using A = FirmwareAssignerScheduler;
using Bridge = FirmwareAssignerAudioBridge;
namespace U = FirmwareUartTrace;
namespace Io = FirmwareAssignerIo;
// In original patch-byte order90..9F, all chosen below the upper ADC stretch.
constexpr std::array<unsigned, 16> patch{64, 0, 0, 0, 0, 100, 40, 0, 0, 0, 100, 0, 20, 100, 20, 20};
constexpr std::uint64_t sliderTime = 300000;
struct Fixture {
    A::State cpu;
    U::State uart;
    Io::Inputs inputs;
};
Fixture warm() {
    Fixture f;
    auto &r = f.cpu.ram;
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned address = Io::panelParameterAddress[i];
        f.inputs.panelAdc[i] = static_cast<std::uint8_t>(4 + 2 * patch[address - 0x90]);
        r[0x58 + i] = r[0x6c + i] = f.inputs.panelAdc[i];
        r[address] = patch[address - 0x90];
    }
    for (unsigned i = 0; i < 4; ++i)
        r[0x68 + i] = f.inputs.directAdc[i];
    f.cpu.io.conversion = f.inputs.directAdc;
    for (unsigned i = 0; i < 6; ++i) {
        r[0x80 + i] = 0x88 + i;
        r[0x88 + i] = 0x80;
    }
    r[0x86] = 0x88;
    r[0x4f] = 0x21;
    r[0xb6] = 2;
    r[0xb8] = r[0x8e] = r[0xc7] = 0x2a; // Eight, pulse, chorus off
    r[0xba] = 0x10;
    r[0xbb] = 1;
    r[0xbc] = 8;
    r[0xbe] = 12;
    r[0xc5] = r[0xc6] = 4;
    r[0xc8] = 0x42;
    r[0xcb] = 0xfc;
    return f;
}
std::vector<A::InputEvent> midi() {
    std::vector<A::InputEvent> result;
    auto message = [&](std::uint64_t t, std::initializer_list<unsigned> bytes) {
        for (auto byte : bytes) {
            result.push_back({t, static_cast<std::uint8_t>(byte)});
            t += 1280;
        }
    };
    message(10000, {0x90, 60, 64});
    message(80000, {0x90, 67, 64});
    message(200000, {0xb0, 64, 127});
    message(250000, {0x80, 60, 0});
    message(280000, {0x80, 67, 0});
    message(600000, {0xb0, 64, 0});
    return result;
}
std::unique_ptr<Engine> sceneEngine(std::span<const S::ByteReady> bytes, unsigned rate,
                                    unsigned factor, bool streaming) {
    auto e = std::make_unique<Engine>();
    ProductFidelityProfile::configureBeforePrepare(*e);
    EngineParameters p;
    auto f = [](unsigned i) { return float(patch[i]) / 127.f; };
    p.lfoRate = f(0);
    p.lfoDelay = f(1);
    p.dcoLfoDepth = f(2);
    p.pwmDepth = f(3);
    p.noiseLevel = f(4);
    p.cutoff = f(5);
    p.resonance = f(6);
    p.envDepth = f(7);
    p.vcfLfoDepth = f(8);
    p.keyFollow = f(9);
    p.vcaLevel = f(10);
    p.attack = f(11);
    p.decay = f(12);
    p.sustain = f(13);
    p.release = f(14);
    p.subLevel = f(15);
    p.range = DcoRange::Eight;
    p.pulseEnabled = true;
    p.sawEnabled = false;
    p.chorus = ChorusMode::Off;
    p.highPass = static_cast<HighPassMode>(3);
    p.pwmSource = PwmSource::Lfo;
    p.vcaMode = VcaMode::Envelope;
    p.envPolarity = EnvPolarity::Normal;
    p.calibration = p.aging = p.chorusNoise = p.velocityDepth = 0;
    ProductFidelityProfile::applyTo(p);
    e->setParameters(p);
    Engine::FirmwareSerialReplayConfiguration replay;
    replay.schedule = bytes;
    replay.streaming = streaming;
    check(e->configureFirmwareSerialReplay(replay),
          "actual DSP accepts continuous sender-derived bytes");
    e->prepare(rate, 128, int(factor));
    return e;
}

std::vector<S::ByteReady> expected(bool unison) {
    // Independently raw-opcode-executed original A-5, explicit warm image,
    // phase37 and ADC restart. These are frame-end scenario coordinates, not
    // measured receive-latch timing. Every unison voice receives the same60.
    if (unison)
        return {{19493, 0x88},  {20773, 60},    {22053, 0x89},  {23333, 60},    {24613, 0x8a},
                {25893, 60},    {27173, 0x8b},  {28453, 60},    {29733, 0x8c},  {31013, 60},
                {32293, 0x8d},  {33573, 60},    {256677, 0x80}, {257957, 0x81}, {259237, 0x82},
                {260517, 0x83}, {261797, 0x84}, {263077, 0x85}};
    return {{20005, 0x88},  {21285, 60},    {89125, 0x89},  {90405, 67},  {211749, 0x86},
            {258725, 0x80}, {288165, 0x81}, {338725, 0x95}, {340005, 20}, {609317, 0x87}};
}
struct PitLedger {
    std::array<bool, 6> needsMsb{};
    std::array<std::uint64_t, 6> firstControl{}, firstMsb{}, firstCv{};
    void observe(const Engine &e) {
        const auto &events = e.firmwareSerialEvents();
        for (std::size_t i = 0; i < events.count; ++i) {
            const auto &v = events.entries[i];
            if (v.kind == S::EventKind::ExternalWrite) {
                if (v.address == 0x1300 || v.address == 0x2300) {
                    unsigned card = (v.address == 0x1300 ? 3 : 0) + (v.value >> 6);
                    if (card < 6) {
                        needsMsb[card] = false;
                        if (!firstControl[card])
                            firstControl[card] = v.states;
                    }
                } else if ((v.address >= 0x1000 && v.address <= 0x1200) ||
                           (v.address >= 0x2000 && v.address <= 0x2200)) {
                    unsigned card = (v.address < 0x2000 ? 3 : 0) + ((v.address & 0x0300) >> 8);
                    if (needsMsb[card] && v.states >= 20773 + 2560ULL * card && !firstMsb[card])
                        firstMsb[card] = v.states;
                    needsMsb[card] = !needsMsb[card];
                }
            } else if (v.kind == S::EventKind::Converter && (v.pa & 0x70) == 0x60 &&
                       (v.pa & 7) < 6) {
                const unsigned card = v.pa & 7;
                if (v.states >= 20773 + 2560ULL * card && !firstCv[card])
                    firstCv[card] = v.states;
            }
        }
    }
};
std::uint64_t pressureRetries = 0;
std::vector<float> composition(unsigned rate, int factor, unsigned chunk, bool unison,
                               bool repeatAfterReset = false) {
    const auto bytes = expected(unison);
    auto fixed = sceneEngine(bytes, rate, factor, false),
         live = sceneEngine({}, rate, factor, true);
    const unsigned total = unsigned((unison ? 360000ULL : 960000ULL) * rate / 4000000ULL);
    std::vector<float> previous;
    for (unsigned repeat = 0; repeat < (repeatAfterReset ? 2u : 1u); ++repeat) {
        auto initial = warm();
        if (unison)
            initial.cpu.ram[0xc8] = 0x46;
        Bridge::State bridge;
        check(Bridge::reset(bridge, initial.cpu, initial.uart),
              "explicit warm sender/receiver reset accepted");
        if (repeat) {
            fixed->reset();
            live->reset();
        }
        A::Configuration configuration;
        configuration.uart.idleBaudGridPhase = 37;
        A::Tables tables;
        const auto input =
            unison ? std::vector<A::InputEvent>{{10000, 0x90},  {11280, 60},  {12560, 64},
                                                {250000, 0x80}, {251280, 60}, {252560, 0}}
                   : midi();
        std::size_t inputCursor = 0;
        std::vector<S::ByteReady> received;
        received.reserve(bytes.size());
        std::vector<float> audio;
        audio.reserve(total * 2);
        bool stepped = false;
        bool sixActive = false;
        PitLedger pit;
        auto advance = [&](std::uint64_t target) {
            for (unsigned guard = 0; guard < 10000; ++guard) {
                // Capacity one deliberately tests drain/resume when more than
                // one completed module frame lies inside an audio block.
                std::array<Bridge::ByteReady, 1> decoded;
                Bridge::Output output{decoded, 0};
                countingAllocations = true;
                const auto result = Bridge::advanceTo(
                    bridge, configuration, tables, initial.inputs,
                    std::span<const A::InputEvent>(input).subspan(inputCursor), target, output);
                countingAllocations = false;
                check(allocations == 0, "live CPU/UART/pin decoder does not allocate");
                inputCursor += result.consumedInputs;
                std::array<S::ByteReady, 1> batch;
                for (std::size_t i = 0; i < output.count; ++i)
                    batch[i] = {decoded[i].states, decoded[i].value};
                countingAllocations = true;
                const bool accepted =
                    live->appendFirmwareSerialBytes(std::span(batch).first(output.count));
                countingAllocations = false;
                check(accepted && allocations == 0,
                      "decoded module bytes append before audio without allocation");
                for (std::size_t i = 0; i < output.count; ++i)
                    received.push_back(batch[i]);
                if (result.status == Bridge::Status::ReachedTarget) {
                    check(bridge.assigner.now == target && bridge.uart.now == target &&
                              bridge.receiver.now == target,
                          "all continuous sender/receiver clocks reach audio boundary");
                    return;
                }
                if (result.status != Bridge::Status::OutputFull &&
                    result.status != Bridge::Status::WorkLimit)
                    std::cerr << "bridge diagnostic status=" << unsigned(result.status)
                              << " source=" << unsigned(result.sourceStatus)
                              << " decoder=" << unsigned(result.decoderStatus)
                              << " target=" << target << " now=" << bridge.assigner.now << '\n';
                check(result.status == Bridge::Status::OutputFull ||
                          result.status == Bridge::Status::WorkLimit,
                      "bridge remains supported while bounded buffers drain");
                ++pressureRetries;
            }
            check(false, "bridge drain terminates");
        };
        for (unsigned frame = 0; frame < total;) {
            const unsigned n = std::min(chunk, total - frame);
            const auto target = std::uint64_t(std::floor((frame + n) * 4000000.L / rate + 1.e-10L));
            if (!unison && !stepped && target >= sliderTime) {
                advance(sliderTime);
                initial.inputs.panelAdc[6] = 44;
                stepped = true;
            }
            advance(target);
            pairedBlock(*fixed, *live, n, &audio);
            if (unison && target > 100000 && target < 200000)
                sixActive |= X::activeCards(*live) == 63;
            if (unison && factor == 1 && chunk == 1)
                pit.observe(*live);
            frame += n;
        }
        check(inputCursor == input.size() && sameBytes(received, bytes),
              "actual continuous A5 and pin decoder match every independent raw-ROM ready "
              "coordinate");
        check(live->pendingFirmwareSerialBytes() == 0,
              "live bridge leaves no queued receiver bytes");
        check(bridge.assigner.ram[0xc1] == bridge.assigner.ram[0xc2] && !bridge.uart.frameActive &&
                  !bridge.uart.txBufferFull,
              "actual sender FIFO and shifter complete the performance");
        if (!unison) {
            check(bridge.assigner.ram[0x95] == 20,
                  "physical slider passes through original ADC processing");
            check(bridge.assigner.foregroundPasses == 142,
                  "independent original foreground pass count exact");
        }
        check(std::any_of(audio.begin(), audio.end(), [](float x) { return std::abs(x) > 1.e-7f; }),
              "full continuous live path produces actual nonzero audio");
        if (unison)
            check(sixActive, "same-note unison actually activates all six physical voice cards");
        if (unison && factor == 1 && chunk == 1) {
            for (unsigned card = 0; card < 6; ++card)
                check(pit.firstControl[card] > 0 && pit.firstMsb[card] > 0 && pit.firstCv[card] > 0,
                      "all six real oscillators have initial control plus post-note-ready divider "
                      "and CV stores");
            if (rate == 48000) {
                std::cout << "unison initial control / first post-note-ready MSB / CV at48k1x:";
                for (unsigned card = 0; card < 6; ++card)
                    std::cout << " card" << card << "=" << pit.firstControl[card] << ","
                              << pit.firstMsb[card] << "," << pit.firstCv[card];
                std::cout << '\n';
            }
        }
        if (repeat)
            check(audio == previous,
                  "reset of sender receiver and engine reproduces exact live audio");
        previous = std::move(audio);
    }
    return previous;
}
void run() {
    for (unsigned rate : {44100u, 48000u, 96000u})
        for (int factor : {1, 4})
            for (bool unison : {false, true}) {
                const auto single = composition(rate, factor, 1, unison);
                const auto blocks =
                    composition(rate, factor, 83, unison, rate == 48000 && factor == 4 && !unison);
                check(single == blocks,
                      "live complete sender/receiver/audio is block-partition invariant");
            }
    check(pressureRetries > 0, "capacity-one live bridge actually exercised output backpressure");
}
} // namespace liveComposition
int main() {
    matrix();
    queueContracts();
    compaction();
    lifecycle();
    liveComposition::run();
    std::cout << "PASS " << assertions << " assertions; allocations=" << allocations
              << "; bridge pressure retries=" << liveComposition::pressureRetries << '\n';
}
