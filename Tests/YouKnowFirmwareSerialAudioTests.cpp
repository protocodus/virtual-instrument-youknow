// Actual-engine serial comparison bridge contract. The independent maintained CPU is the
// bus oracle; no ADC, instruction, PIT or converter events are inferred from
// host note callbacks. Neutral raw inputs and byte-ready origins are explicit.
// Complete product circuits use nominal Character/Aging zero; nominal converter
// leakage remains enabled except the isolated abandoned-DAC ownership witness.
// That witness retains a completed physical counter write across serial restart,
// passes the abandoned converter time, and checks a differing stale code never
// reaches its capacitor. The optional ENVhold fixture integrates physical edges
// independently with declared100ohm/10nF RC and explicit bias/leakage/charge.
// This validates a comparison profile, not IC1 delivery or a cold boot image.
// Synthetic ready timestamps denote completed RXB bytes at4MHz CPU states.
// Original NEC NMOS timing and raw B-2 decoding are independently covered by
// YouKnowFirmwareSerialTraceTests and the hash-verified firmware audit tool.
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include <algorithm>
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
void check(bool x, const char *what) {
    ++assertions;
    if (!x) {
        ++failureKinds[what];
        if (failures++ < 16)
            std::cerr << what << '\n';
    }
}
namespace youknow {
struct YouKnowTestAccess {
    static bool running(const auto &d) { return d.pitState == Engine::Dco::PitState::running; }
    static auto tables(const Engine &e) { return e.firmwareSerialTables_; }
    static const auto &ram(const Engine &e) { return e.firmwareControlState_.ram; }
    static const auto &voice(const Engine &e, unsigned card) { return e.voices_[card]; }
    static auto dcoEvents(const Engine &e, unsigned card) {
        return std::span(e.firmwareSerialDcoEvents_[card].data(), e.firmwareSerialDcoCounts_[card]);
    }
    static bool isMsb(const auto &e) { return e.kind == Engine::SerialDcoEvent::Kind::Msb; }
    static bool isPitch(const auto &e) { return e.kind == Engine::SerialDcoEvent::Kind::PitchCv; }
    static const auto &holds(const Engine &e) { return e.envelopeHolds_; }
    static constexpr double holdZero() { return Engine::VoiceVcaSignalLaw::holdStandoffVolts; }
    static constexpr double holdSpan() { return Engine::VoiceVcaControlLaw::controlFullScaleVolts; }
    static double rate(const Engine &e) { return e.oversampledRate_; }
    static bool sustain(const Engine &e) { return e.sustainPedalDown_; }
    static auto physical(const Engine &e) {
        std::vector<double> x;
        for (unsigned card = 0; card < 6; ++card) {
            const auto &v = e.voices_[card];
            const auto &d = v.dco;
            for (double a :
                 {double(d.divider), double(d.pendingDivider), double(d.pendingDividerValid),
                  double(d.pitState), double(d.pitOutHigh), d.pitClocksToEvent, d.rampValue,
                  d.rampSlopePerSecond, d.resetSecondsRemaining, double(v.dcoCv),
                  double(v.dcoCvTarget), double(v.cutoffCounts), double(v.cutoffCountsTarget),
                  double(v.vcaControl), double(v.vcaControlTarget), double(v.active),
                  double(v.freewheeling), double(v.envelope.level)})
                x.push_back(a);
            for (double a : v.filter.state)
                x.push_back(a);
        }
        return x;
    }
};
} // namespace youknow
using X = YouKnowTestAccess;
using E = std::tuple<unsigned, std::uint64_t, unsigned, unsigned, unsigned, unsigned, unsigned,
                     unsigned, unsigned, unsigned>;
E normalized(const S::Event &e) {
    return {unsigned(e.kind), e.states,    e.pc, e.address, e.value,
            e.card,           e.phaseBits, e.pa, e.pb,      e.portc};
}
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
                    x.alternateEa, x.alternateV, x.pc, x.sp, x.carry, x.skip, x.pa, x.pb,
                    x.portc) == std::tie(y.a, y.b, y.c, y.d, y.e, y.h, y.l, y.ea, y.v, y.alternateA,
                                         y.alternateB, y.alternateC, y.alternateD, y.alternateE,
                                         y.alternateH, y.alternateL, y.alternateEa, y.alternateV,
                                         y.pc, y.sp, y.carry, y.skip, y.pa, y.pb, y.portc);
}
EngineParameters patch() {
    EngineParameters p;
    p.calibration = 0;
    p.aging = 0;
    p.chorusNoise = 0;
    p.chorus = ChorusMode::Off;
    p.keyMode = KeyMode::Unison;
    p.polyphony = 6;
    p.velocityDepth = 0;
    p.benderDcoDepth = p.benderVcfDepth = p.benderLfoDepth = 0;
    p.lfoDelay = 0;
    p.attack = 0;
    p.decay = .05f;
    p.sustain = .6f;
    p.release = .02f;
    p.cutoff = .6f;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    ProductFidelityProfile::applyTo(p);
    return p;
}
std::vector<S::ByteReady> score() {
    std::vector<S::ByteReady> b;
    for (unsigned card = 0; card < 6; ++card) {
        b.push_back({20000 + 2560 * card, std::uint8_t(0x88 + card)});
        b.push_back({21280 + 2560 * card, 48});
    }
    b.push_back({80000, 0x86});
    for (unsigned card = 0; card < 6; ++card)
        b.push_back({100000 + 1280 * card, std::uint8_t(0x80 + card)});
    b.push_back({140000, 0x87});
    b.push_back({180000, 0x88});
    b.push_back({181280, 55});
    b.push_back({220000, 0x80});
    return b;
}
std::unique_ptr<Engine> create(double rate, int factor,
                               Engine::FirmwareSerialReplayConfiguration config,
                               bool holds = false) {
    auto e = std::make_unique<Engine>();
    ProductFidelityProfile::configureBeforePrepare(*e);
    e->setParameters(patch());
    check(e->configureFirmwareSerialReplay(config), "configuration accepted");
    if (holds) {
        std::array<EnvelopeHoldCircuit::Configuration, 6> c;
        for (auto &x : c)
            x = {100., 1.e-10, 2.e-10, 1.e-13};
        check(e->configureEnvelopeHolds(c), "hold fixture accepted");
    }
    e->prepare(rate, 256, factor);
    e->setParameters(patch());
    return e;
}
struct Oracle {
    S::State state;
    FirmwareControlTrace::Tables tables;
    std::size_t cursor = 0;
    std::vector<S::Event> events;
};
void advance(Oracle &o, const Engine::FirmwareSerialReplayConfiguration &c, std::uint64_t target) {
    o.events.clear();
    for (unsigned guard = 0; guard < 100; ++guard) {
        S::Events events;
        auto r = S::advanceTo(o.state, o.tables, c.inputs, c.cpu, c.schedule.subspan(o.cursor),
                              target, events);
        o.cursor += r.consumedBytes;
        o.events.insert(o.events.end(), events.entries.begin(),
                        events.entries.begin() + events.count);
        check(r.status == S::Status::ReachedTarget || r.status == S::Status::OutputFull,
              "oracle runs supported scene");
        if (r.status == S::Status::ReachedTarget)
            return;
        if (r.status != S::Status::OutputFull)
            return;
    }
    check(false, "oracle drain guard");
}
void initialAdapter(const Engine &e) {
    const auto &r = e.firmwareSerialState().control.ram;
    constexpr std::array<unsigned, 8> raw{132, 0, 255, 0, 0, 0, 0, 0},
        processed{128, 0, 255, 0, 0, 0, 0, 0};
    for (unsigned i = 0; i < 8; ++i) {
        check(r[0x80 + i] == raw[i], "initial raw history explicit");
        check(r[0x88 + i] == processed[i], "initial processed ADC exact");
    }
    check(r[0x10] == 0 && r[0x11] == 0 && r[0x33] == 0, "initial gates idle");
    check(e.firmwareSerialAudioStates() == 0, "prepare CPU origin zero");
}
struct HoldReference {
    double v = X::holdZero(), bus = X::holdZero();
    bool selected = false;
    void advance(double dt) {
        if (selected) {
            double equilibrium = bus - 1e-10 * 100.;
            v = equilibrium + (v - equilibrium) * std::exp(-dt / 1e-6);
        } else
            v -= 3e-10 / 1e-8 * dt;
    }
    void inhibit() {
        if (selected)
            v += 1e-13 / 1e-8;
        selected = false;
    }
};
unsigned observedMsb = 0, observedPitch = 0, observedDormant = 0, observedMultiPit = 0,
         observedInhibitEnable = 0, observedSustain = 0;
double maximumTimeError = 0, maximumHoldError = 0;
void oracleScene(double rate, int factor, bool holds = false) {
    auto bytes = score();
    Engine::FirmwareSerialReplayConfiguration cfg;
    cfg.schedule = bytes;
    auto e = create(rate, factor, cfg, holds);
    initialAdapter(*e);
    Oracle o{e->firmwareSerialState(), X::tables(*e), 0, {}};
    std::array<HoldReference, 6> hold;
    std::array<unsigned, 6> rawLsb{};
    std::array<bool, 6> rawNeedsMsb{};
    std::array<unsigned, 6> runningSeen{}, rampChanged{};
    std::array<double, 6> previousRamp{};
    double audioEnergy = 0;
    const unsigned n = unsigned(rate * .08);
    double previousStates = 0;
    for (unsigned i = 0; i < n; ++i) {
        float l, r;
        e->process(&l, &r, 1);
        check(std::isfinite(l) && std::isfinite(r), "actual audio finite");
        audioEnergy += double(l) * l + double(r) * r;
        check(e->firmwareSerialStatus() == S::Status::ReachedTarget,
              "engine replay remains supported");
        auto target = e->firmwareSerialState().now;
        advance(o, cfg, target);
        check(sameState(e->firmwareSerialState(), o.state),
              "actual engine CPU/RAM identical to independent trace");
        check(X::ram(*e) == o.state.control.ram, "audio adapter RAM identical to trace RAM");
        double expected = double(i + 1) * 4000000. / rate;
        maximumTimeError =
            std::max(maximumTimeError, std::abs(expected - e->firmwareSerialAudioStates()));
        if (e->getOversamplingFactor() == 1) {
            const auto &actual = e->firmwareSerialEvents();
            check(actual.count == o.events.size(), "complete internal interval event count");
            if (actual.count == o.events.size())
                for (unsigned j = 0; j < actual.count; ++j)
                    check(normalized(actual.entries[j]) == normalized(o.events[j]),
                          "actual emitted bus event exact");
        }
        if (e->getOversamplingFactor() == 1)
            for (const auto &event : o.events) {
                if (event.kind == K::ExternalWrite) {
                    if (event.address == 0x1300 || event.address == 0x2300) {
                        unsigned card = (event.address == 0x1300 ? 3 : 0) + (event.value >> 6);
                        if (card < 6)
                            rawNeedsMsb[card] = false;
                    } else if ((event.address >= 0x1000 && event.address <= 0x1200) ||
                               (event.address >= 0x2000 && event.address <= 0x2200)) {
                        unsigned card =
                            (event.address < 0x2000 ? 3 : 0) + ((event.address & 0x0300) >> 8);
                        if (!rawNeedsMsb[card]) {
                            rawLsb[card] = event.value;
                            rawNeedsMsb[card] = true;
                        } else {
                            rawNeedsMsb[card] = false;
                            unsigned count = rawLsb[card] + 256 * event.value;
                            if (!count)
                                count = 65536;
                            const auto &d = X::voice(*e, card).dco;
                            check(d.divider == count ||
                                      (d.pendingDividerValid && d.pendingDivider == count),
                                  "raw address decoded independently reaches correct physical "
                                  "counter");
                        }
                    }
                }
                if (event.kind == K::Converter && (event.pa & 0x70) == 0x60 && (event.pa & 7) < 6)
                    check(X::voice(*e, event.pa & 7).dcoCvTarget == float(event.value),
                          "raw PA decoded independently reaches correct physical pitch hold");
            }
        const auto &ram = o.state.control.ram;
        for (unsigned card = 0; card < 6; ++card) {
            const auto &v = X::voice(*e, card);
            if (X::running(v.dco)) {
                ++runningSeen[card];
                if (v.dco.rampValue != previousRamp[card])
                    ++rampChanged[card];
            }
            previousRamp[card] = v.dco.rampValue;
            check(v.envelope.gate == bool(ram[0x10] & (1u << card)),
                  "actual voice gate follows RAM");
            check(v.envelope.running == bool(ram[0x11] & (1u << card)),
                  "actual voice envelope running follows RAM");
            auto list = X::dcoEvents(*e, card);
            unsigned pit = 0;
            for (const auto &ev : list) {
                check(ev.position >= 0 && ev.position <= 1,
                      "fractional physical event in interval");
                if (X::isMsb(ev)) {
                    ++observedMsb;
                    ++pit;
                    check(v.dco.divider == ev.value ||
                              (v.dco.pendingDividerValid && v.dco.pendingDivider == ev.value),
                          "completed PIT bytes reach actual count register");
                } else if (X::isPitch(ev)) {
                    ++observedPitch;
                    check(v.dcoCvTarget == float(ev.value),
                          "actual DCO capacitor target uses emitted DAC code");
                } else
                    ++pit;
            }
            if (pit > 1)
                ++observedMultiPit;
            if (!v.active && !list.empty())
                ++observedDormant;
            if (v.sustained)
                ++observedSustain;
        }
        if (holds) {
            double at = previousStates;
            bool inhibited = false;
            for (auto &ev : o.events) {
                if (ev.kind != K::Inhibit && ev.kind != K::Converter)
                    continue;
                double time = double(ev.states);
                for (auto &h : hold)
                    h.advance((time - at) / 4000000.);
                at = time;
                if (ev.kind == K::Inhibit) {
                    for (auto &h : hold)
                        h.inhibit();
                    inhibited = true;
                } else if ((ev.pa & 0x70) == 0x30 && (ev.pa & 7) < 6) {
                    if (inhibited)
                        ++observedInhibitEnable;
                    for (auto &h : hold)
                        h.inhibit();
                    auto &h = hold[ev.pa & 7];
                    h.selected = true;
                    float target = std::clamp(float(ev.value) / 4095.f, 0.f, 1.f);
                    h.bus = X::holdZero() + double(target) * X::holdSpan();
                }
            }
            for (auto &h : hold)
                h.advance((e->firmwareSerialAudioStates() - at) / 4000000.);
            for (unsigned card = 0; card < 6; ++card) {
                auto &actual = X::holds(*e)[card];
                maximumHoldError =
                    std::max(maximumHoldError, std::abs(actual.volts() - hold[card].v));
                check(std::abs(actual.volts() - hold[card].v) < 1e-8,
                      "actual hold capacitor follows independent exact edge integral");
                check(actual.selected() == hold[card].selected, "actual hold mux selection exact");
            }
        }
        previousStates = e->firmwareSerialAudioStates();
    }
    for (unsigned card = 0; card < 6; ++card)
        check(runningSeen[card] > 0 && rampChanged[card] > 0,
              "all six physical PITs run and charge actual ramp capacitors");
    check(audioEnergy > 1e-9, "explicit serial note scene generates actual nonzero audio");
    check(o.state.adcInterrupts > 0 && o.state.serialInterrupts == bytes.size(),
          "actual stream includes ADC plus every serial byte");
}
struct Render {
    std::vector<float> l, r;
};
Render render(Engine &e, unsigned frames, unsigned block, bool hostCalls = false) {
    Render a;
    a.l.resize(frames);
    a.r.resize(frames);
    for (unsigned at = 0; at < frames;) {
        if (hostCalls) {
            e.noteOn(90, 1);
            e.noteOff(90);
            e.setSustainPedal(true);
            e.setSustainPedal(false);
        }
        unsigned n = std::min(frames - at, block);
        e.process(a.l.data() + at, a.r.data() + at, int(n));
        at += n;
    }
    return a;
}
void partitionLifecycle(double rate, int factor) {
    auto bytes = score();
    Engine::FirmwareSerialReplayConfiguration cfg;
    cfg.schedule = bytes;
    auto a = create(rate, factor, cfg), b = create(rate, factor, cfg),
         c = create(rate, factor, cfg);
    unsigned n = unsigned(rate * .065);
    auto one = render(*a, n, 1), blocks = render(*b, n, 83), host = render(*c, n, 17, true);
    check(one.l == blocks.l && one.r == blocks.r, "actual audio block partition bit exact");
    check(one.l == host.l && one.r == host.r,
          "host note and sustain cannot mutate explicit replay");
    check(sameState(a->firmwareSerialState(), b->firmwareSerialState()),
          "partition CPU state exact");
    check(X::physical(*a) == X::physical(*b),
          "partition physical oscillator/filter/capacitors exact");
    check(X::physical(*a) == X::physical(*c), "host note mutation leaves physical state exact");
    check(!a->configureFirmwareSerialReplay(cfg), "prepared schedule replacement rejected");
    a->reset();
    check(a->firmwareSerialState().now == 0, "reset restarts byte-ready origin");
    auto replay = render(*a, n, 83);
    check(one.l == replay.l && one.r == replay.r, "reset actual audio replay bit exact");
    a->prepare(rate, 256, factor);
    a->setParameters(patch());
    auto prepared = render(*a, n, 83);
    check(one.l == prepared.l && one.r == prepared.r, "prepare actual audio replay bit exact");
}
void interruptedPit() {
    std::vector<S::ByteReady> bytes{{0, 0x88}, {1280, 61}};
    Engine::FirmwareSerialReplayConfiguration cfg;
    cfg.schedule = bytes;
    auto base = create(192000, 1, cfg);
    Oracle o{base->firmwareSerialState(), X::tables(*base), 0, {}};
    advance(o, cfg, 30000);
    unsigned seen = 0;
    std::uint64_t msb = 0, dac = 0;
    unsigned count = 0, lsb = 0;
    for (auto &ev : o.events) {
        if (ev.kind == K::ExternalWrite && ev.address == 0x2000) {
            if (!seen++) {
                lsb = ev.value;
            } else if (!msb) {
                msb = ev.states;
                count = lsb + 256 * ev.value;
            }
        }
        if (msb && !dac && ev.kind == K::Converter && (ev.pa & 0x77) == 0x60)
            dac = ev.states;
    }
    check(msb && dac > msb, "witness has actual completed PIT preceding DAC");
    bytes.push_back({msb, 0x80});
    cfg.schedule = bytes;
    auto engine = create(192000, 1, cfg);
    // Isolate whether a DAC write occurred from the separately tested physical leakage ramp.
    auto noDroop = patch();
    noDroop.enableConverterHoldDroop = false;
    engine->setParameters(noDroop);
    Oracle interrupted{engine->firmwareSerialState(), X::tables(*engine), 0, {}};
    advance(interrupted, cfg, dac);
    std::uint64_t restart = 0;
    bool converter = false, persisted = false;
    for (auto &ev : interrupted.events) {
        if (ev.kind == K::StackReset && ev.states > msb)
            restart = ev.states;
        if (ev.kind == K::ExternalWrite && ev.address == 0x2000 && ev.states == msb)
            persisted = true;
        if (ev.kind == K::Converter && (ev.pa & 0x77) == 0x60 && ev.states >= msb)
            converter = true;
    }
    check(restart > msb && restart < dac && persisted && !converter,
          "oracle proves PIT write survives abandoned pitch-DAC transaction");
    float cv = X::voice(*engine, 0).dcoCvTarget;
    for (auto &ev : interrupted.events)
        if (ev.kind == K::Converter && (ev.pa & 0x77) == 0x60 && ev.states < msb)
            cv = float(ev.value);
    unsigned abandonedCode = 0;
    for (auto &ev : o.events)
        if (ev.kind == K::Converter && ev.states == dac)
            abandonedCode = ev.value;
    check(float(abandonedCode) != cv,
          "abandoned DAC code would measurably change current hold target");
    const unsigned frames = unsigned(std::ceil(double(dac) * 192000. / 4000000.));
    render(*engine, frames, 1);
    advance(interrupted, cfg, engine->firmwareSerialState().now);
    check(std::none_of(
              interrupted.events.begin(), interrupted.events.end(),
              [](const auto &ev) { return ev.kind == K::Converter && (ev.pa & 0x77) == 0x60; }),
          "no replacement DAC through stale scheduled commit time");
    const auto &v = X::voice(*engine, 0);
    check(v.dco.divider == count || (v.dco.pendingDividerValid && v.dco.pendingDivider == count),
          "actual oscillator retains completed PIT without matching DAC");
    check(v.dcoCvTarget == cv, "abandoned DAC does not mutate actual pitch capacitor target");
    std::cout << "{\"witness_msb\":" << msb << ",\"restart\":" << restart
              << ",\"abandoned_dac\":" << dac << ",\"count\":" << count << "}\n";
}
void rateTransition() {
    std::vector<S::ByteReady> bytes{{200000, 0x88}, {201280, 61}};
    Engine::FirmwareSerialReplayConfiguration cfg;
    cfg.schedule = bytes;
    auto e = create(48000, 4, cfg);
    Oracle o{e->firmwareSerialState(), X::tables(*e), 0, {}};
    render(*e, 64, 1);
    auto before = e->firmwareSerialState();
    auto physical = X::physical(*e);
    double time = e->firmwareSerialAudioStates();
    check(!e->setOversamplingFactor(1), "idle oversampling change waits for existing safety fade");
    check(sameState(before, e->firmwareSerialState()) && time == e->firmwareSerialAudioStates(),
          "rate request preserves CPU RAM partial instruction and absolute time");
    check(X::physical(*e) == physical,
          "rate request preserves physical oscillator and capacitor state");
    unsigned processed = 64;
    while (e->getOversamplingFactor() != 1 && processed < 2000) {
        render(*e, 1, 1);
        ++processed;
        advance(o, cfg, e->firmwareSerialState().now);
        check(sameState(e->firmwareSerialState(), o.state),
              "actual transition preserves continuous CPU and pending work");
    }
    check(e->getOversamplingFactor() == 1, "idle oversampling change eventually applies");
    render(*e, 4000, 83);
    processed += 4000;
    advance(o, cfg, e->firmwareSerialState().now);
    check(sameState(e->firmwareSerialState(), o.state),
          "post-rate actual audio continues same absolute byte-ready trace");
    check(std::abs(e->firmwareSerialAudioStates() - processed * 4000000. / 48000.) < 1e-9,
          "rate change keeps4MHz absolute time");
}
void noAllocationAndReject() {
    auto bytes = score();
    auto original = bytes;
    Engine::FirmwareSerialReplayConfiguration cfg;
    cfg.schedule = bytes;
    auto e = create(48000, 4, cfg);
    std::array<float, 256> l{}, r{};
    allocationCount = 0;
    countAllocations = true;
    for (unsigned i = 0; i < 12; ++i)
        e->process(l.data(), r.data(), 256);
    countAllocations = false;
    check(allocationCount == 0,
          "actual callback CPU adapter and physical event integration allocate nothing");
    check(bytes.size() == original.size() &&
              std::equal(bytes.begin(), bytes.end(), original.begin(),
                         [](auto a, auto b) { return a.states == b.states && a.value == b.value; }),
          "immutable caller owned schedule remains unchanged");
    for (unsigned bad = 0; bad < 4; ++bad) {
        Engine invalid;
        auto c = cfg;
        if (bad == 0)
            c.initialAdc.statesUntilConversion = 0;
        if (bad == 1)
            c.initialAdc.channel = 4;
        if (bad == 2)
            c.cpu.adc.accessBoundary = static_cast<FirmwareAdcTrace::PeripheralAccessBoundary>(2);
        if (bad == 3) {
            std::swap(bytes[0], bytes[1]);
            c.schedule = bytes;
        }
        check(!invalid.configureFirmwareSerialReplay(c),
              "invalid configuration rejected before prepare");
    }
}
int main() {
    noAllocationAndReject();
    interruptedPit();
    rateTransition();
    for (auto [rate, factor] :
         {std::pair{8000., 1}, std::pair{32000., 1}, std::pair{44100., 1}, std::pair{48000., 1},
          std::pair{48000., 4}, std::pair{96000., 1}, std::pair{192000., 1}}) {
        oracleScene(rate, factor, rate == 32000);
        partitionLifecycle(rate, factor);
    }
    check(observedMsb > 0 && observedPitch > 0, "physical PIT and DAC checks nonvacuous");
    check(observedMultiPit > 0, "multiple physical PIT bytes within interval observed");
    check(observedDormant > 0, "inactive freewheeling card stores observed");
    check(observedSustain > 0, "sustain gate-off running behavior observed");
    check(observedInhibitEnable > 0, "inhibit and enable share tested interval");
    for (auto &[s, n] : failureKinds)
        std::cerr << n << ' ' << s << '\n';
    std::cout << "{\"assertions\":" << assertions << ",\"failures\":" << failures
              << ",\"msb_checks\":" << observedMsb << ",\"pitch_checks\":" << observedPitch
              << ",\"dormant\":" << observedDormant << ",\"multiple_pit\":" << observedMultiPit
              << ",\"sustain\":" << observedSustain
              << ",\"hold_same_interval\":" << observedInhibitEnable
              << ",\"maximum_time_error_states\":" << maximumTimeError
              << ",\"maximum_hold_error_volts\":" << maximumHoldError << "}\n";
    return failures ? 1 : 0;
}
