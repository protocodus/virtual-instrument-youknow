// Actual parameter-byte -> ROM -> physical audio bridge. RXB-ready origins are
// explicit fixture data, not a reconstruction of the IC1 assigner schedule.
// Original A5 0BFB/0C2B complements the stored patch switch bytes before B2
// 0162..01C5 drives PF/IC40. The independent schema below records that polarity.
// https://github.com/ErroneousBosh/j106roms/tree/26926a04ff1939106820313e71e34b4ca2f67070
// Range timing uses a separate discrete 8 MHz 74HC161 oracle, not engine helpers.
// PF acts at its exact CPU event fraction; SAW/HPF/chorus routing is consumed at
// the containing internal interval endpoint (an explicit audio-grid policy).
// C54 charging is independently integrated from nominal 399k/200k/100k source
// resistances for 16/8/4 foot. This covers charge continuity at nominal Character
// with a nonunity voltage coordinate, not new reset-transistor characterization.
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13
// CPU event expectations use the separately ROM-qualified maintained trace.
// Complete product circuits run with explicit static raw8 inputs and nominal
// Character/Aging, including nominal converter leakage except the isolated
// capacitor-charge fixture. No gain, delay or tolerance is fitted.
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <tuple>
#include <vector>
#include <map>
#include <new>
#include <cstdlib>
static bool countAllocations = false;
static std::size_t allocationCount = 0;
void *operator new(std::size_t n) {
    if (countAllocations)
        ++allocationCount;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
using namespace youknow;
using S = FirmwareSerialTrace;
using K = S::EventKind;
using Engine = YouKnowEngine;
unsigned assertions = 0, failures = 0;
std::map<std::string, unsigned> failureKinds;
void check(bool x, const char *m) {
    ++assertions;
    if (!x) {
        ++failureKinds[m];
        if (failures++ < 16)
            std::cerr << m << '\n';
    }
}
namespace youknow {
struct YouKnowTestAccess {
    static const auto &tables(const Engine &e) { return e.firmwareSerialParameterTables_; }
    static const auto &circuit(const Engine &e) { return e.firmwareSerialCircuitParameters_; }
    static unsigned ic40(const Engine &e) { return e.firmwareSerialIc40_; }
    static std::uint64_t ic40Time(const Engine &e) { return e.firmwareSerialIc40States_; }
    static double clock(const Engine &e) { return e.rangeClockClocksToFallingEdge_; }
    static const auto &voice(const Engine &e, unsigned n) { return e.voices_[n]; }
    static auto range(const Engine &e) { return e.firmwareSerialRange_; }
    static double internalRate(const Engine &e) { return e.oversampledRate_; }
    static const auto &events(const Engine &e, unsigned c) { return e.firmwareSerialDcoEvents_[c]; }
    static unsigned eventCount(const Engine &e, unsigned c) {
        return e.firmwareSerialDcoCounts_[c];
    }
    static bool rangeEvent(const auto &e) { return e.kind == Engine::SerialDcoEvent::Kind::Range; }
    static unsigned chorusSelection(const Engine &e) {
        return unsigned(e.chorus_.hardwareModeSelection_);
    }
    static unsigned chorusRunning(const Engine &e) { return unsigned(e.chorus_.runningMode_); }
    static void isolateRamp(Engine &e) {
        for (auto &v : e.voices_) {
            auto &d = v.dco;
            d.divider = 60000;
            d.pendingDividerValid = false;
            d.pitState = Engine::Dco::PitState::running;
            d.pitOutHigh = true;
            d.pitClocksToEvent = e.rangeClockClocksToFallingEdge_ + 10000;
            d.rampValue = -.3;
            d.rampSlopePerSecond = 500;
            d.resetSecondsRemaining = 0;
            d.physicalResetActive = false;
            d.positiveRailHeld = false;
            d.coldInitialLoadPending = false;
            d.renderScale = .91f;
            v.rampCurrentScale = 1;
            v.dcoCv = v.dcoCvTarget;
        }
    }
    static double pwmTarget(const Engine &e) { return e.pwmVoltsTarget_; }
    static double bend(const Engine &e) {
        return e.firmwareControlState_.ram[0x68] + 256u * e.firmwareControlState_.ram[0x69];
    }
    static auto physical(const Engine &e) {
        std::vector<double> v;
        v.push_back(e.rangeClockClocksToFallingEdge_);
        for (unsigned c = 0; c < 6; ++c) {
            const auto &x = e.voices_[c];
            for (double d :
                 {double(x.dco.divider), double(x.dco.pendingDivider),
                  double(x.dco.pendingDividerValid), x.dco.pitClocksToEvent, x.dco.rampValue,
                  x.dco.rampSlopePerSecond, double(x.dcoCv), double(x.dcoCvTarget),
                  double(x.cutoffCounts), double(x.vcaControl), double(x.envelope.level)})
                v.push_back(d);
            for (double d : x.filter.state)
                v.push_back(d);
        }
        return v;
    }
};
}
using X = YouKnowTestAccess;
using ET = std::tuple<unsigned, std::uint64_t, unsigned, unsigned, unsigned, unsigned, unsigned,
                      unsigned, unsigned, unsigned>;
ET normalized(const S::Event &e) {
    return {unsigned(e.kind), e.states,    e.pc, e.address, e.value,
            e.card,           e.phaseBits, e.pa, e.pb,      e.portc};
}
bool sameState(const S::State &a, const S::State &b) {
    const auto &x = a.registers;
    const auto &y = b.registers;
    return a.control.ram == b.control.ram && a.control.adcComplete == b.control.adcComplete &&
           a.registers.portf == b.registers.portf && a.now == b.now && a.rxb == b.rxb &&
           a.ordinal == b.ordinal && a.adc.elapsedStates == b.adc.elapsedStates &&
           a.adc.conversion == b.adc.conversion && a.adc.channel == b.adc.channel &&
           a.adc.anm == b.adc.anm && a.adc.request == b.adc.request &&
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
                    x.alternateEa, x.alternateV, x.pc, x.sp, x.carry, x.skip, x.pa, x.pb,
                    x.portc) == std::tie(y.a, y.b, y.c, y.d, y.e, y.h, y.l, y.ea, y.v, y.alternateA,
                                         y.alternateB, y.alternateC, y.alternateD, y.alternateE,
                                         y.alternateH, y.alternateL, y.alternateEa, y.alternateV,
                                         y.pc, y.sp, y.carry, y.skip, y.pa, y.pb, y.portc);
}
unsigned wireOne(unsigned range, bool saw, bool pulse, unsigned chorus, bool offModeOne = false) {
    unsigned stored = (1u << range) | (unsigned(pulse) << 3) | (unsigned(saw) << 4) |
                      (unsigned(chorus == 0) << 5) | (unsigned(chorus == 1 || offModeOne) << 6);
    return (~stored) & 127u;
}
unsigned wireTwo(unsigned hpf, bool manual = true, bool inverse = false, bool gate = false) {
    return (~(unsigned(manual) | (unsigned(inverse) << 1) | (unsigned(gate) << 2) |
              ((3 - hpf) << 3))) &
           31u;
}
std::vector<S::ByteReady> scene() {
    std::vector<S::ByteReady> b{{2000, 0x88}, {3280, 48}};
    auto param = [&](unsigned t, unsigned cmd, unsigned v) {
        b.push_back({t, std::uint8_t(cmd)});
        b.push_back({t + 1280, std::uint8_t(v)});
    };
    param(20000, 0x95, 25);
    param(26000, 0x9b, 30);
    param(30000, 0x97, 100);
    param(34000, 0x90, 110);
    param(40000, 0xa0, 90);
    param(46000, 0xa2, 90);
    param(52000, 0x93, 80);
    param(58000, 0x9f, 50);
    param(64000, 0x9d, 90);
    param(70000, 0x9e, 20);
    param(76000, 0x8e, wireOne(2, true, true, 2));
    param(82000, 0x8f, wireTwo(3, false, true, true));
    param(88000, 0x8e, wireOne(0, true, false, 0, true));
    param(94000, 0x8e, wireOne(1, true, true, 1));
    param(100000, 0x8f, wireTwo(0));
    param(106000, 0x8e, wireOne(2, false, true, 0));
    param(112000, 0x8e, wireOne(0, true, true, 2));
    param(118000, 0x8e, wireOne(1, true, true, 1));
    param(124000, 0x9a, 70);
    param(130000, 0x96, 60);
    param(136000, 0x91, 5);
    param(142000, 0x92, 18);
    b.push_back({160000, 0x80});
    b.push_back({180000, 0x88});
    b.push_back({181280, 55});
    b.push_back({220000, 0x80});
    return b;
}
EngineParameters patch() {
    EngineParameters p;
    p.calibration = p.aging = p.chorusNoise = 0;
    p.sawEnabled = true;
    p.pulseEnabled = true;
    p.subLevel = .2f;
    p.keyMode = KeyMode::Poly1;
    p.polyphony = 6;
    p.velocityDepth = 0;
    p.attack = 0;
    p.decay = .1f;
    p.release = .1f;
    p.cutoff = .7f;
    p.chorus = ChorusMode::One;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    ProductFidelityProfile::applyTo(p);
    return p;
}
Engine::FirmwareSerialReplayConfiguration config(std::span<const S::ByteReady> b) {
    Engine::FirmwareSerialReplayConfiguration c;
    c.schedule = b;
    c.inputs = {{132, 0, 255, 196, 0, 196, 0, 0}};
    return c;
}
std::unique_ptr<Engine> create(double rate, int factor,
                               const Engine::FirmwareSerialReplayConfiguration &c,
                               EngineParameters p = patch()) {
    auto e = std::make_unique<Engine>();
    ProductFidelityProfile::configureBeforePrepare(*e);
    e->setParameters(p);
    check(e->configureFirmwareSerialReplay(c), "live configuration accepted");
    e->prepare(rate, 256, factor);
    e->setParameters(p);
    return e;
}
struct Oracle {
    S::State state;
    S::Tables tables;
    std::size_t cursor = 0;
    std::vector<S::Event> events;
};
void advance(Oracle &o, const Engine::FirmwareSerialReplayConfiguration &c, std::uint64_t target) {
    o.events.clear();
    for (unsigned guard = 0; guard < 500; ++guard) {
        S::Events e;
        auto r = S::advanceTo(o.state, o.tables, c.inputs, c.cpu, c.schedule.subspan(o.cursor),
                              target, e);
        o.cursor += r.consumedBytes;
        o.events.insert(o.events.end(), e.entries.begin(), e.entries.begin() + e.count);
        check(r.status == S::Status::ReachedTarget || r.status == S::Status::OutputFull,
              "independent live trace completes");
        if (r.status == S::Status::ReachedTarget)
            return;
        if (r.status != S::Status::OutputFull)
            return;
    }
    check(false, "live oracle guard");
}
// Physical discrete counter: previous count15 loads current PF preset on the
// next8MHz tick. Otherwise count increments; entering15 emits a TP5 fall.
// A tick exactly coincident with PF is processed first under the declared policy.
struct Counter161 {
    unsigned count = 15, selected = 12;
    std::uint64_t tick = 0;
    void until(double t) {
        auto n = std::uint64_t(std::floor(t + 1e-8));
        while (tick < n) {
            ++tick;
            count = count == 15 ? selected : count + 1;
        }
    }
    void pf(unsigned value) { selected = value == 192 ? 14 : value == 64 ? 12 : 8; }
    double phase(double t) const {
        auto c = count;
        auto n = tick;
        do {
            ++n;
            c = c == 15 ? selected : c + 1;
        } while (c != 15);
        return (double(n) - t) / double(16 - selected);
    }
};
unsigned pfEvents = 0, ic40Writes = 0, deferredLatchFrames = 0, rangeApplicationChecks = 0,
         physicalCutoffChanges = 0, physicalBendChanges = 0, pulseOffWrites = 0;
double clockError = 0, chargeError = 0;
void liveScene(double rate, int factor) {
    auto bytes = scene();
    auto c = config(bytes);
    auto e = create(rate, factor, c);
    Oracle o{e->firmwareSerialState(), X::tables(*e), 0, {}};
    Counter161 clock;
    unsigned expectedIc40 = X::ic40(*e);
    std::uint64_t expectedIc40Time = 0;
    DcoRange expectedRange = DcoRange::Eight;
    double lastCutoff = X::voice(*e, 0).cutoffCountsTarget;
    double lastPitch = X::voice(*e, 0).dcoCvTarget;
    double energy = 0;
    const unsigned frames = unsigned(rate * .065);
    for (unsigned n = 0; n < frames; ++n) {
        float l, r;
        e->process(&l, &r, 1);
        energy += double(l) * l + double(r) * r;
        check(std::isfinite(l) && std::isfinite(r), "live audio finite");
        check(e->firmwareSerialStatus() == S::Status::ReachedTarget,
              "live audio CPU remains supported");
        advance(o, c, e->firmwareSerialState().now);
        check(sameState(o.state, e->firmwareSerialState()),
              "live audio CPU RAM flags and pending work exact to independent trace");
        if (e->getOversamplingFactor() == 1) {
            const auto &ev = e->firmwareSerialEvents();
            check(ev.count == o.events.size(), "live bus event count exact");
            if (ev.count == o.events.size())
                for (unsigned i = 0; i < ev.count; ++i)
                    check(normalized(ev.entries[i]) == normalized(o.events[i]),
                          "live bus event words and timestamps exact");
        }
        for (auto &ev : o.events) {
            if (ev.kind == K::PortFWrite) {
                ++pfEvents;
                clock.until(double(ev.states) * 2);
                clock.pf(ev.value);
                expectedRange = ev.value == 192  ? DcoRange::Four
                                : ev.value == 64 ? DcoRange::Eight
                                                 : DcoRange::Sixteen;
            }
            if (ev.kind == K::ExternalWrite && ev.address == 0x3000) {
                ++ic40Writes;
                expectedIc40 = ev.value;
                expectedIc40Time = ev.states;
            }
            if (ev.kind == K::Converter && (ev.pa & 0x77) == 0x57 && ev.value == 0)
                ++pulseOffWrites;
        }
        const double rawTime = double(n + 1) * 8000000. / rate;
        clock.until(rawTime);
        const double expectedPhase = clock.phase(rawTime);
        clockError = std::max(clockError, std::abs(expectedPhase - X::clock(*e)));
        check(std::abs(expectedPhase - X::clock(*e)) < 2e-7,
              "shared IC35 agrees with independent8MHz discrete counter");
        check(X::range(*e) == expectedRange, "physical range follows actual PF latch");
        check(X::ic40(*e) == expectedIc40 && X::ic40Time(*e) == expectedIc40Time,
              "actual IC40 bus latch updates at external store only");
        const auto &p = X::circuit(*e);
        check(p.sawEnabled == !(expectedIc40 & 16) &&
                  unsigned(p.highPass) == 3 - ((expectedIc40 >> 2) & 3) &&
                  p.chorus == ((expectedIc40 & 1)   ? ChorusMode::Off
                               : (expectedIc40 & 2) ? ChorusMode::Two
                                                    : ChorusMode::One),
              "actual circuit switches follow proven active-low IC40 routes");
        check(p.pulseEnabled, "pulse waveform remains governed by physical PWM hold");
        check(X::chorusSelection(*e) ==
                      unsigned((expectedIc40 & 2) ? ChorusMode::Two : ChorusMode::One) &&
                  X::chorusRunning(*e) == X::chorusSelection(*e),
              "actual chorus selector follows IC40 independently while muted");
        if (o.state.control.ram[0x46] != expectedIc40)
            ++deferredLatchFrames;
        if (e->getOversamplingFactor() == 1)
            for (const auto &ev : o.events)
                if (ev.kind == K::Converter) {
                    if ((ev.pa & 0x77) == 0x57)
                        check(
                            std::abs(X::pwmTarget(*e) - float(-.8 + 6.8 * ev.value / 4095.)) < 1e-4,
                            "actual PWM target follows physical converter code including Off zero");
                    if ((ev.pa & 0x70) == 0x30 && (ev.pa & 7) < 6)
                        check(std::abs(X::voice(*e, ev.pa & 7).vcaControlTarget -
                                       float(ev.value) / 4095) < 1e-4,
                              "actual envelope target follows converter code");
                }
        for (unsigned card = 0; card < 6; ++card)
            for (unsigned i = 0; i < X::eventCount(*e, card); ++i) {
                const auto &ev = X::events(*e, card)[i];
                if (X::rangeEvent(ev)) {
                    ++rangeApplicationChecks;
                    check(ev.position >= 0 && ev.position <= 1 &&
                              ev.value == unsigned(expectedRange),
                          "all six physical cards receive fractional PF event");
                }
            }
        const auto &v = X::voice(*e, 0);
        if (v.cutoffCountsTarget != lastCutoff)
            ++physicalCutoffChanges;
        if (v.dcoCvTarget != lastPitch && n > unsigned(rate * .012) && n < unsigned(rate * .035) &&
            X::bend(*e) != 0)
            ++physicalBendChanges;
        lastCutoff = v.cutoffCountsTarget;
        lastPitch = v.dcoCvTarget;
    }
    check(energy > 1e-9, "live parameter stream produces nonzero actual audio");
    check(o.state.serialInterrupts == bytes.size() && o.state.adcInterrupts > 0,
          "every live byte and ADC stream serviced");
}
struct Audio {
    std::vector<float> l, r;
};
Audio render(Engine &e, unsigned count, unsigned block) {
    Audio a;
    a.l.resize(count);
    a.r.resize(count);
    for (unsigned i = 0; i < count;) {
        unsigned n = std::min(block, count - i);
        e.process(a.l.data() + i, a.r.data() + i, int(n));
        i += n;
    }
    return a;
}
void partitions(double rate, int factor) {
    auto b = scene();
    auto c = config(b);
    auto a = create(rate, factor, c), d = create(rate, factor, c);
    auto x = render(*a, unsigned(rate * .065), 1), y = render(*d, unsigned(rate * .065), 83);
    check(x.l == y.l && x.r == y.r, "live parameter audio block invariant");
    check(sameState(a->firmwareSerialState(), d->firmwareSerialState()) &&
              X::physical(*a) == X::physical(*d),
          "live parameter CPU counter and capacitor state block invariant");
    a->reset();
    auto z = render(*a, unsigned(rate * .065), 31);
    check(x.l == z.l && x.r == z.r, "live parameter reset replay exact");
}

// The actual PF event splits a charged C54 interval. No timer edge or DAC event
// occurs inside the ISR interval. The nominal source resistors are independent
// schematic data: 399k/200k/100k for16/8/4 foot. Nonunity renderScale ensures
// coordinate voltage is retained. Component tolerances are disabled only here.
void chargeContinuity() {
    constexpr double resistance[3] = {399000., 200000., 100000.};
    for (double rate : {32000., 44100., 192000.})
        for (unsigned old = 0; old < 3; ++old)
            for (unsigned next = 0; next < 3; ++next) {
                if (old == next)
                    continue;
                std::vector<S::ByteReady> b{{0, 0x8e},
                                            {1280, std::uint8_t(wireOne(next, true, true, 1))}};
                auto c = config(b);
                auto p = patch();
                p.range = DcoRange(old);
                p.enableConverterHoldDroop = false;
                auto e = create(rate, 1, c, p);
                Oracle o{e->firmwareSerialState(), X::tables(*e), 0, {}};
                advance(o, c, 2500);
                std::uint64_t at = 0;
                for (auto &ev : o.events)
                    if (ev.kind == K::PortFWrite)
                        at = ev.states;
                check(at > 0, "charge witness has a real PF write");
                unsigned frames = unsigned(std::ceil(double(at) * rate / 4000000.)) - 1;
                render(*e, frames, 31);
                X::isolateRamp(*e);
                const double start = double(frames) * 4000000. / rate;
                const double fraction = (double(at) - start) * rate / 4000000.;
                check(fraction >= 0 && fraction <= 1,
                      "charge witness PF inside actual audio interval");
                float l, r;
                e->process(&l, &r, 1);
                const double expected =
                    -.3 +
                    500. / rate * (fraction + (1 - fraction) * resistance[old] / resistance[next]);
                for (unsigned card = 0; card < 6; ++card) {
                    const auto &v = X::voice(*e, card);
                    chargeError = std::max(chargeError, std::abs(v.dco.rampValue - expected));
                    check(std::abs(v.dco.rampValue - expected) < 3e-11,
                          "C54 charge integrates old and new physical current at fractional PF");
                    check(std::abs(v.dco.rampSlopePerSecond -
                                   500 * resistance[old] / resistance[next]) < 1e-9,
                          "C54 new current follows schematic resistor ratio");
                    check(v.dco.renderScale == .91f && v.rampCurrentScale == 1 &&
                              v.dco.divider == 60000 && !v.dco.physicalResetActive &&
                              v.dco.resetSecondsRemaining == 0,
                          "PF preserves held charge coordinate and timer without resetting it");
                }
            }
}
// Change just one payload with the same ready-time schedule. For bus-switched
// routes, audio is exactly equal before the *first* possible physical event;
// after the event the complete engine output must measurably differ.
void audioMutations() {
    struct Case {
        const char *name;
        unsigned command, a, b;
    };
    const std::array<Case, 7> cases{
        {{"saw", 0x8e, wireOne(1, true, true, 1), wireOne(1, false, true, 1)},
         {"pulse", 0x8e, wireOne(1, true, true, 1), wireOne(1, true, false, 1)},
         {"range", 0x8e, wireOne(1, true, true, 1), wireOne(2, true, true, 1)},
         {"hpf", 0x8f, wireTwo(0), wireTwo(3)},
         {"chorus", 0x8e, wireOne(1, true, true, 1), wireOne(1, true, true, 0)},
         {"cutoff", 0x95, 100, 10},
         {"envelope", 0x9d, 100, 10}}};
    for (auto test : cases) {
        std::vector<S::ByteReady> ba{{0, 0x88},
                                     {1280, 48},
                                     {80000, std::uint8_t(test.command)},
                                     {81280, std::uint8_t(test.a)}};
        auto bb = ba;
        bb.back().value = std::uint8_t(test.b);
        auto ca = config(ba), cb = config(bb);
        auto a = create(48000, 1, ca), b = create(48000, 1, cb);
        Oracle oa{a->firmwareSerialState(), X::tables(*a), 0, {}},
            ob{b->firmwareSerialState(), X::tables(*b), 0, {}};
        advance(oa, ca, 100000);
        advance(ob, cb, 100000);
        std::uint64_t first = 100000;
        // A value-dependent handler branch can shift a different converter
        // before the named destination. Compare complete physical bus ledgers,
        // including unchanged codes whose store times differ. RAM-only writes
        // cannot set this causal audio boundary.
        auto bus = [](const auto &events) {
            std::vector<S::Event> result;
            for (const auto &event : events)
                if (event.kind == K::PortFWrite || event.kind == K::ExternalWrite ||
                    event.kind == K::Inhibit || event.kind == K::Converter)
                    result.push_back(event);
            return result;
        };
        const auto baEvents = bus(oa.events), bbEvents = bus(ob.events);
        const auto common = std::min(baEvents.size(), bbEvents.size());
        std::size_t i = 0;
        for (; i < common; ++i)
            if (normalized(baEvents[i]) != normalized(bbEvents[i])) {
                first = std::min(baEvents[i].states, bbEvents[i].states);
                break;
            }
        if (i == common && baEvents.size() != bbEvents.size())
            first = (i < baEvents.size() ? baEvents[i] : bbEvents[i]).states;
        check(first < 100000, "mutation has actual physical event");
        auto x = render(*a, 7200, 83), y = render(*b, 7200, 31);
        unsigned before = unsigned(std::floor(double(first) * 48000. / 4000000.));
        check(std::equal(x.l.begin(), x.l.begin() + before, y.l.begin()) &&
                  std::equal(x.r.begin(), x.r.begin() + before, y.r.begin()),
              "audio unchanged before actual physical route/converter event");
        double diff = 0;
        for (unsigned i = before; i < x.l.size(); ++i)
            diff += std::pow(double(x.l[i]) - y.l[i], 2) + std::pow(double(x.r[i]) - y.r[i], 2);
        if (!(diff > 1e-10))
            std::cerr << "mutation missing " << test.name << " diff=" << diff << '\n';
        check(diff > 1e-10, "scheduled parameter affects actual complete audio path");
    }
}
void diagnosticStop() {
    std::vector<S::ByteReady> b{{0, 0x8f}, {1280, 64}, {2560, 0xa3}, {3840, 1}};
    auto c = config(b);
    auto e = create(48000, 1, c);
    auto audio = render(*e, 200, 31);
    check(e->firmwareSerialStatus() == S::Status::UnsupportedPath &&
              e->firmwareSerialState().registers.pc == 0x24b,
          "armed A3 stops before diagnostic instruction");
    check(e->firmwareSerialState().control.ram[1] == 0, "armed A3 cannot write test-mode RAM");
    check(std::all_of(audio.l.begin(), audio.l.end(), [](float x) { return std::isfinite(x); }),
          "unsupported diagnostic leaves finite audio");
}

void noAllocation() {
    auto bytes = scene();
    auto cfg = config(bytes);
    auto a = create(48000, 4, cfg);
    std::array<float, 256> l{}, r{};
    allocationCount = 0;
    countAllocations = true;
    for (unsigned n = 0; n < 13; ++n)
        a->process(l.data(), r.data(), 256);
    countAllocations = false;
    check(allocationCount == 0 && a->firmwareSerialState().serialInterrupts == bytes.size(),
          "all live parameter callbacks allocate nothing");
}

void profileEntry() {
    std::vector<S::ByteReady> b{{0, 0x88},     {1280, 48},
                                {4000, 0x90},  {5280, 80},
                                {8000, 0x91},  {9280, 32},
                                {12000, 0x92}, {13280, 90},
                                {16000, 0x9c}, {17280, 60},
                                {20000, 0x9e}, {21280, 70},
                                {24000, 0x8f}, {25280, std::uint8_t(wireTwo(3))}};
    auto c = config(b);
    for (double rate : {8000., 48000.}) {
        auto direct = create(rate, 1, c);
        auto late = std::make_unique<Engine>();
        ProductFidelityProfile::configureBeforePrepare(*late);
        late->setParameters(patch());
        check(late->configureFirmwareSerialReplay(c), "reset-entry serial configuration accepted");
        late->selectConverterTimingProfile(Engine::ConverterTimingProfile::MeasuredChartGeometry);
        late->prepare(rate, 256, 1);
        late->setParameters(patch());
        late->selectConverterTimingProfile(Engine::ConverterTimingProfile::FirmwareSerialReplay);
        late->reset();
        check(X::internalRate(*late) >= 32000,
              "reset-entry serial profile enforces physical event rate floor");
        const auto &a = X::tables(*direct);
        const auto &d = X::tables(*late);
        check(a.dcoLfoDepth == d.dcoLfoDepth && a.lfoRate == d.lfoRate &&
                  a.decayRelease == d.decayRelease && a.delayFade == d.delayFade &&
                  d.lfoRate[80] > 0 && d.decayRelease[60] > 0 && d.delayFade[2] > 0,
              "all table regions prepared before later serial reset entry");
        auto x = render(*direct, unsigned(rate * .04), 31),
             y = render(*late, unsigned(rate * .04), 83);
        check(direct->firmwareSerialStatus() == S::Status::ReachedTarget &&
                  late->firmwareSerialStatus() == S::Status::ReachedTarget,
              "reset-entry table commands all remain supported");
        check(sameState(direct->firmwareSerialState(), late->firmwareSerialState()),
              "reset-entry table commands match direct serial CPU state");
        check(x.l == y.l && x.r == y.r && X::physical(*direct) == X::physical(*late),
              "reset-entry actual audio and capacitor state equal direct serial preparation");
        auto chart = std::make_unique<Engine>();
        ProductFidelityProfile::configureBeforePrepare(*chart);
        chart->setParameters(patch());
        chart->selectConverterTimingProfile(Engine::ConverterTimingProfile::MeasuredChartGeometry);
        chart->prepare(rate, 256, 1);
        chart->setParameters(patch());
        late->selectConverterTimingProfile(Engine::ConverterTimingProfile::MeasuredChartGeometry);
        late->reset();
        check(X::internalRate(*late) == X::internalRate(*chart),
              "reverse reset entry restores chart processing rate");
        late->noteOn(48, 1.f);
        chart->noteOn(48, 1.f);
        auto aChart = render(*chart, unsigned(rate * .025), 31);
        auto bChart = render(*late, unsigned(rate * .025), 83);
        check(aChart.l == bChart.l && aChart.r == bChart.r &&
                  X::physical(*chart) == X::physical(*late),
              "reverse reset restores host routes and complete chart audio");
    }
}

int main() {
    profileEntry();
    noAllocation();
    chargeContinuity();
    audioMutations();
    diagnosticStop();
    for (auto [rate, factor] :
         {std::pair{8000., 1}, std::pair{32000., 1}, std::pair{44100., 1}, std::pair{48000., 1},
          std::pair{48000., 4}, std::pair{96000., 1}, std::pair{192000., 1}}) {
        liveScene(rate, factor);
        partitions(rate, factor);
    }
    check(pfEvents > 0 && ic40Writes > 0 && deferredLatchFrames > 0 && rangeApplicationChecks > 0,
          "physical latch tests nonvacuous");
    check(physicalCutoffChanges > 0 && physicalBendChanges > 0 && pulseOffWrites > 0,
          "live cutoff bend and pulse converter paths exercised");
    for (auto &[m, n] : failureKinds)
        std::cerr << n << ' ' << m << '\n';
    std::cout << "{\"assertions\":" << assertions << ",\"failures\":" << failures
              << ",\"pf_events\":" << pfEvents << ",\"ic40_writes\":" << ic40Writes
              << ",\"deferred_latch_frames\":" << deferredLatchFrames
              << ",\"range_application_checks\":" << rangeApplicationChecks
              << ",\"cutoff_changes\":" << physicalCutoffChanges
              << ",\"bend_changes\":" << physicalBendChanges
              << ",\"pulse_off_writes\":" << pulseOffWrites
              << ",\"max_clock_phase_error\":" << clockError
              << ",\"max_charge_error\":" << chargeError << "}\n";
    return failures ? 1 : 0;
}
