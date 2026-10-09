// Original A-5 normal foreground regression, from independent raw-ROM execution.
// IC1 A-5 SHA256 d43cce5578ee2f16b27c8b06bff30743e3e2dffc796d033811e565d5d578c52e.
// Original listing:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic1.txt
// NEC Stock500375 (April1987), printed3-4,7-3/4,9-5/7,11-8,12-21/46:
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// Fixed warm panel/RAM and RXB-ready times are explicit scenarios, not cold boot,
// pin receiver latch timing, or a fitted foreground period. The short-note pair
// proves scan-phase dependence, not a universal minimum note length. Expected
// RAM/registers/TXB times came from a separate original-byte interpreter; no ROM
// or research-directory dependency is needed to run this test.
#include "../Source/DSP/YouKnowFirmwareAssignerScheduler.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

static unsigned assertions = 0;
static bool watch = false;
static unsigned allocations = 0;
void *operator new(std::size_t size) {
    if (watch)
        ++allocations;
    if (auto *value = std::malloc(size ? size : 1))
        return value;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *value) noexcept { std::free(value); }
void operator delete[](void *value) noexcept { std::free(value); }
void operator delete(void *value, std::size_t) noexcept { std::free(value); }
void operator delete[](void *value, std::size_t) noexcept { std::free(value); }
static void check(bool condition, const char *message) {
    ++assertions;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
namespace schedulerScenarioRegression {
using S = youknow::FirmwareAssignerScheduler;
namespace U = youknow::FirmwareUartTrace;
namespace Io = youknow::FirmwareAssignerIo;
using ByteWrites = std::vector<std::pair<std::uint64_t, unsigned>>;
struct Fixture {
    S::State state;
    U::State uart;
    S::Configuration configuration;
    // No display-button action occurs here; these explicitly supplied display
    // values are unused. Missing patch RAM remains unavailable, never zero-filled.
    S::Tables tables;
    Io::Inputs inputs;
    Fixture() {
        inputs.panelAdc.fill(132);
        state.io.conversion = {0, 0, 0, 255};
        auto &ram = state.ram;
        std::fill(ram.begin() + 0x58, ram.begin() + 0x68, 132);
        std::fill(ram.begin() + 0x6c, ram.begin() + 0x7c, 132);
        ram[0x6b] = 255;
        for (unsigned card = 0; card < 6; ++card) {
            ram[0x80 + card] = 0x88 + card;
            ram[0x88 + card] = 0x80;
        }
        ram[0x86] = 0x88;
        std::fill(ram.begin() + 0x90, ram.begin() + 0xa0, 64);
        ram[0x4f] = 0x21;
        ram[0xb6] = 2;
        ram[0x8e] = ram[0xb8] = ram[0xc7] = 0x2a;
        ram[0xba] = 16;
        ram[0xbb] = 1;
        ram[0xbc] = 8;
        ram[0xbe] = 12;
        ram[0xc5] = ram[0xc6] = 4;
        ram[0xc8] = 0x42;
        ram[0xcb] = 0xfc;
    }
};
struct Run {
    Fixture fixture;
    std::vector<S::Event> events;
    std::vector<U::Event> wire;
    unsigned outputStops = 0;
};
Run execute(Fixture fixture, std::span<const S::InputEvent> incoming, std::uint64_t end,
            std::uint64_t chunk = 100000, std::size_t wireCapacity = 256,
            bool reserveMostCpuSlots = false) {
    Run run{std::move(fixture), {}, {}, 0};
    S::Events events;
    std::array<U::Event, 256> wire{};
    U::EventBuffer output{wire.data(), wireCapacity, 0};
    std::size_t consumed = 0;
    unsigned attempts = 0;
    std::uint64_t target = 0;
    do {
        target = std::min(end, target + chunk);
        S::Status status;
        do {
            const auto prefix = reserveMostCpuSlots ? events.entries.size() - 8 : 0;
            events.count = prefix;
            watch = true;
            const auto result = S::advanceTo(
                run.fixture.state, run.fixture.uart, run.fixture.configuration, run.fixture.tables,
                run.fixture.inputs, incoming.subspan(consumed), target, events, output);
            watch = false;
            consumed += result.consumedInputs;
            status = result.status;
            run.events.insert(run.events.end(), events.entries.begin() + prefix,
                              events.entries.begin() + events.count);
            run.wire.insert(run.wire.end(), wire.begin(), wire.begin() + output.count);
            events.count = output.count = 0;
            if (status == S::Status::OutputFull)
                ++run.outputStops;
            check(status == S::Status::ReachedTarget || status == S::Status::OutputFull ||
                      status == S::Status::InstructionBudget,
                  "normal scenario remains inside the covered execution graph");
            check(++attempts < 200000, "resumable execution makes bounded progress");
        } while (status != S::Status::ReachedTarget);
    } while (target < end);
    check(consumed == incoming.size(), "all supplied RXB events consumed exactly once");
    check(allocations == 0, "actual foreground/ADC/RX/TX execution allocates no heap memory");
    return run;
}
ByteWrites txb(const Run &run) {
    ByteWrites result;
    for (const auto &event : run.events)
        if (event.kind == S::EventKind::TxBufferWrite)
            result.emplace_back(event.states, event.value);
    return result;
}
std::vector<unsigned> values(const Run &run) {
    std::vector<unsigned> result;
    for (auto [time, value] : txb(run)) {
        (void)time;
        result.push_back(value);
    }
    return result;
}
auto registers(const S::Registers &r) {
    return std::tuple{r.a,          r.b,           r.c,          r.d,          r.e,
                      r.h,          r.l,           r.ea,         r.v,          r.alternateA,
                      r.alternateB, r.alternateC,  r.alternateD, r.alternateE, r.alternateH,
                      r.alternateL, r.alternateEa, r.alternateV, r.pc,         r.sp,
                      r.carry,      r.skip,        r.halfCarry,  r.zero,       r.l0,
                      r.l1};
}
auto pending(const S::Pending &p) {
    return std::tuple{p.kind, p.start, p.remaining, p.address, p.returnPc, p.savedPsw, p.skipped};
}
auto peripheral(const Io::Peripheral &p) {
    return std::tuple{
        p.muxLatch, p.portF, p.portB, p.anm, p.conversion, p.channel, p.statesUntilConversion,
        p.request};
}
auto uart(const U::State &u) {
    return std::tuple{
        u.now,         u.transmitEnabled, u.txBufferFull,       u.txBuffer,     u.fst,
        u.frameActive, u.frameByte,       u.bitIndex,           u.frameOrdinal, u.frameStart,
        u.nextBit,     u.idleLaunch,      u.stopCenterObserved, u.portC,        u.txd,
        u.moduleRxD,   u.midiLogic};
}
bool sameState(const Run &a, const Run &b) {
    const auto &x = a.fixture.state;
    const auto &y = b.fixture.state;
    return x.ram == y.ram && x.patchRam == y.patchRam &&
           x.patchRamAvailable == y.patchRamAvailable &&
           registers(x.registers) == registers(y.registers) &&
           pending(x.pending) == pending(y.pending) && peripheral(x.io) == peripheral(y.io) &&
           uart(a.fixture.uart) == uart(b.fixture.uart) &&
           std::tuple{x.now, x.foregroundPasses, x.mkh,          x.eiDeferred, x.interruptEnabled,
                      x.fsr, x.receiveError,     x.rxBufferFull, x.rxBuffer} ==
               std::tuple{y.now,          y.foregroundPasses, y.mkh,
                          y.eiDeferred,   y.interruptEnabled, y.fsr,
                          y.receiveError, y.rxBufferFull,     y.rxBuffer};
}
bool sameEvents(const Run &a, const Run &b) {
    return a.events.size() == b.events.size() && a.wire.size() == b.wire.size() &&
           std::equal(a.events.begin(), a.events.end(), b.events.begin(),
                      [](const auto &x, const auto &y) {
                          return std::tuple{x.kind, x.states, x.pc, x.address, x.value} ==
                                 std::tuple{y.kind, y.states, y.pc, y.address, y.value};
                      }) &&
           std::equal(a.wire.begin(), a.wire.end(), b.wire.begin(),
                      [](const auto &x, const auto &y) {
                          return std::tuple{x.kind,  x.states,   x.frameOrdinal,
                                            x.value, x.bitIndex, x.pinLevels} ==
                                 std::tuple{y.kind,  y.states,   y.frameOrdinal,
                                            y.value, y.bitIndex, y.pinLevels};
                      });
}
void run() {
    constexpr std::array<S::InputEvent, 5> common{
        {{100, 0x90}, {1380, 60}, {2660, 127}, {10420, 60}, {11700, 0}}};
    const auto reference = execute(Fixture{}, common, 40006);
    check(txb(reference) == ByteWrites{{5555, 0x88}, {6230, 60}, {15920, 0x80}},
          "literal raw-ROM continuous note-on/running-status-zero TXB times");
    const auto &state = reference.fixture.state;
    constexpr std::array<std::uint8_t, 256> expectedRam{
        253, 136, 60,  253, 128, 0,   0,   0,   0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   60,  1,   0,   0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   144, 161, 0,   0,   0,   0,   0,   0,   0,  0,  132, 132, 132, 132, 132, 132, 132,
        132, 132, 132, 132, 132, 132, 132, 132, 132, 0,   0,  0,  255, 132, 132, 132, 132, 132, 132,
        132, 132, 132, 132, 132, 132, 132, 132, 132, 132, 0,  0,  0,   0,   137, 138, 139, 140, 141,
        136, 136, 0,   176, 128, 128, 128, 128, 128, 42,  0,  64, 64,  64,  64,  64,  64,  64,  64,
        64,  64,  64,  64,  64,  64,  64,  64,  0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  2,  0,   42,  0,   16,  1,   237, 0,
        12,  0,   0,   5,   5,   0,   0,   4,   4,   42,  66, 0,  0,   254, 0,   1,   0,   0,   0,
        0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,  0,  0,   0,   0,   0,   0,   0,   0,
        0,   0,   9,   10,  64,  136, 255, 128, 12,  10,  64, 44, 11,  82,  255, 0,   0,   8,   0,
        117, 10,  0,   86,  5,   17,  1,   17,  0};
    check(state.ram == expectedRam,
          "all RAM including physical stack matches independent execution");
    const auto &r = state.registers;
    check(std::array<unsigned, 11>{r.a, r.b, r.c, r.d, r.e, r.h, r.l, r.ea, r.v, r.pc, r.sp} ==
              std::array<unsigned, 11>{1, 16, 0, 255, 105, 255, 125, 12288, 255, 404, 65535},
          "final working registers match independent execution");
    check(std::array<unsigned, 9>{r.alternateA, r.alternateB, r.alternateC, r.alternateD,
                                  r.alternateE, r.alternateH, r.alternateL, r.alternateEa,
                                  r.alternateV} ==
              std::array<unsigned, 9>{247, 6, 253, 0, 0, 255, 107, 0, 255},
          "alternate register bank matches independent execution");
    check(r.carry && r.halfCarry && !r.zero && !r.skip && !r.l0 && !r.l1 && state.mkh == 4 &&
              state.interruptEnabled && state.eiDeferred == 0 && state.foregroundPasses == 5 &&
              state.io.statesUntilConversion == 88 && state.io.channel == 0 &&
              state.io.conversion == std::array<std::uint8_t, 4>{132, 132, 132, 132},
          "original flags, real backedges and ADC phase match independent execution");
    for (const std::uint64_t chunk : {1, 7, 16, 83, 4096}) {
        const auto split = execute(Fixture{}, common, 40006, chunk);
        check(sameState(reference, split), "all persisted CPU/peripheral state is chunk invariant");
        check(sameEvents(reference, split),
              "all CPU writes and wire observations are chunk invariant");
    }
    const auto drained = execute(Fixture{}, common, 40006, 40006, 8, true);
    check(drained.outputStops > 20, "small CPU and UART outputs exercise actual backpressure");
    check(sameState(reference, drained) && sameEvents(reference, drained),
          "drain/retry preserves all committed events and pending state");
    // Both use 1280-state-spaced bytes, and a 2560-state note payload interval.
    // A 256-state translation changes whether the bitmap is observed by the scan.
    constexpr std::array<S::InputEvent, 5> seen{
        {{0, 0x90}, {1280, 60}, {2560, 127}, {3840, 60}, {5120, 0}}};
    constexpr std::array<S::InputEvent, 5> lost{
        {{256, 0x90}, {1536, 60}, {2816, 127}, {4096, 60}, {5376, 0}}};
    check(txb(execute(Fixture{}, seen, 40006)) ==
              ByteWrites{{6261, 0x88}, {6936, 60}, {15920, 0x80}},
          "one short-note phase reaches actual foreground allocation");
    check(txb(execute(Fixture{}, lost, 40000)).empty(),
          "other short-note phase coalesces on/off before allocation");
    {
        Fixture f;
        f.state.ram[0x46] = 145;
        f.state.ram[0x47] = 0;
        f.state.ram[0xc8] = (f.state.ram[0xc8] & ~6) | 2;
        check(txb(execute(f, {}, 60006)) ==
                  ByteWrites{
                      {4375, 136}, {4991, 67}, {7104, 137}, {7779, 64}, {9855, 138}, {10819, 60}},
              "original descending bitmap chord allocation and queue timing");
    }
    {
        Fixture f;
        f.state.ram[0x46] = 181;
        f.state.ram[0x47] = 42;
        f.state.ram[0xc8] = (f.state.ram[0xc8] & ~6) | 2;
        check(txb(execute(f, {}, 60002)) == ByteWrites{{4320, 136},
                                                       {4936, 73},
                                                       {7017, 137},
                                                       {7692, 71},
                                                       {9704, 138},
                                                       {10320, 69},
                                                       {12887, 139},
                                                       {13562, 67},
                                                       {15377, 140},
                                                       {16052, 65},
                                                       {18055, 141},
                                                       {18730, 64}},
              "original descending bitmap poly_overfull allocation and queue timing");
    }
    {
        Fixture f;
        f.state.ram[0x46] = 145;
        f.state.ram[0x47] = 0;
        f.state.ram[0xc8] = (f.state.ram[0xc8] & ~6) | 6;
        check(txb(execute(f, {}, 60008)) == ByteWrites{{3850, 136},
                                                       {4525, 67},
                                                       {5424, 137},
                                                       {6705, 67},
                                                       {7991, 138},
                                                       {9262, 67},
                                                       {10817, 139},
                                                       {11820, 67},
                                                       {13103, 140},
                                                       {14383, 67},
                                                       {15666, 141},
                                                       {16951, 67}},
              "original descending bitmap unison_chord allocation and queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 3> input{{{100, 145}, {1380, 60}, {2660, 127}}};
        check(txb(execute(f, input, 40002)) == ByteWrites{},
              "raw-ROM wrong_channel parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 7> input{{{100, 144},
                                                      {1380, 60},
                                                      {2660, 248},
                                                      {3940, 127},
                                                      {5220, 61},
                                                      {6500, 254},
                                                      {7780, 127}}};
        check(txb(execute(f, input, 40009)) ==
                  ByteWrites{{13087, 136}, {13762, 61}, {15752, 137}, {16427, 60}},
              "raw-ROM realtime parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 7> input{
            {{100, 144}, {1380, 60}, {2660, 127}, {3940, 60}, {5220, 127}, {6500, 60}, {7780, 0}}};
        check(txb(execute(f, input, 40000)) == ByteWrites{{6245, 136}, {7129, 60}, {16610, 128}},
              "raw-ROM repeated parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 5> input{
            {{100, 176}, {1380, 64}, {2660, 1}, {3940, 64}, {5220, 0}}};
        check(txb(execute(f, input, 40005)) == ByteWrites{{7080, 135}, {8404, 134}},
              "raw-ROM sustain parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 3> input{{{100, 176}, {1380, 1}, {2660, 44}}};
        check(txb(execute(f, input, 40005)) == ByteWrites{{6667, 162}, {7283, 44}},
              "raw-ROM mod parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 5> input{
            {{100, 224}, {1380, 0}, {2660, 0}, {3940, 0}, {5220, 64}}};
        check(txb(execute(f, input, 40005)) ==
                  ByteWrites{{7246, 160}, {7921, 0}, {15836, 161}, {16452, 127}},
              "raw-ROM pitch parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 9> input{{{100, 176},
                                                      {1380, 125},
                                                      {2660, 0},
                                                      {3940, 145},
                                                      {5220, 60},
                                                      {6500, 127},
                                                      {7780, 176},
                                                      {9060, 124},
                                                      {10340, 0}}};
        check(txb(execute(f, input, 40002)) == ByteWrites{},
              "raw-ROM channelmode parser and continuous queue timing");
    }
    {
        Fixture f;
        f.configuration.uart.idleBaudGridPhase = 37;
        constexpr std::array<S::InputEvent, 7> input{
            {{100, 240}, {1380, 65}, {2660, 50}, {3940, 0}, {5220, 5}, {6500, 20}, {7780, 247}}};
        check(txb(execute(f, input, 40003)) == ByteWrites{{9827, 149}, {10808, 20}},
              "raw-ROM sysexcutoff parser and continuous queue timing");
    }
}
} // namespace schedulerScenarioRegression
namespace schedulerCoreRegression {
using S = youknow::FirmwareAssignerScheduler;
namespace U = youknow::FirmwareUartTrace;
namespace Io = youknow::FirmwareAssignerIo;
struct Fixture {
    S::State state;
    U::State uart;
    S::Configuration config;
    S::Tables tables;
    Io::Inputs inputs;
    S::Events events;
    std::array<U::Event, 32> wire{};
    U::EventBuffer output{wire.data(), wire.size(), 0};
    Fixture() {
        state.interruptEnabled = false;
        state.mkh = 7;
    }
    S::Result run(std::uint64_t target, std::span<const S::InputEvent> incoming = {}) {
        return S::advanceTo(state, uart, config, tables, inputs, incoming, target, events, output);
    }
};
void run(void (*check)(bool, const char *)) {
    // Original NEC3-4/12-46: logical tests set Z, but preserve HC/CY.
    // Use actual A-5 instruction addresses, not a synthetic opcode seam.
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned flags = 0; flags < 4; ++flags) {
            const bool cy = (flags & 1) != 0, hc = (flags & 2) != 0;
            for (unsigned op = 0; op < 7; ++op) {
                Fixture f;
                auto &r = f.state.registers;
                r.a = a;
                r.carry = cy;
                r.halfCarry = hc;
                r.zero = true;
                constexpr std::array<unsigned, 7> pc{0x015f, 0x04c3, 0x03ee, 0x025c,
                                                     0x04a8, 0x0187, 0x08b1};
                constexpr std::array<unsigned, 7> duration{7, 7, 4, 4, 8, 7, 7};
                r.pc = pc[op];
                check(f.run(duration[op]).status == S::Status::ReachedTarget,
                      "real arithmetic instruction completes at original time");
                if (op == 0) {
                    check(r.a == (a + 12) % 256 && r.carry == (a + 12 > 255) &&
                              r.halfCarry == (a % 16 + 12 > 15) && r.zero == (r.a == 0),
                          "ADI sets independent carry/half-carry/zero");
                } else if (op == 1) {
                    check(r.a == a && r.carry == (a <= 128) && r.halfCarry == (a % 16 == 0) &&
                              r.zero == (a == 129) && r.skip == (a > 128),
                          "GTI uses full subtract-with-one borrow");
                } else if (op == 2 || op == 3) {
                    const bool down = op == 3;
                    const unsigned result = down ? (a + 255) % 256 : (a + 1) % 256;
                    check(r.a == result && r.carry == cy && r.zero == (result == 0) &&
                              r.halfCarry == (down ? a % 16 == 0 : a % 16 == 15) &&
                              r.skip == (down ? a == 0 : a == 255),
                          "INR/DCR preserve CY while setting HC/Z/SK");
                } else if (op == 4) {
                    check(r.a == (256 - a) % 256 && r.carry == cy && r.halfCarry == hc && r.zero,
                          "NEGA preserves all arithmetic flags");
                } else {
                    const unsigned mask = op == 5 ? 1 : 128;
                    check(r.a == a && r.carry == cy && r.halfCarry == hc &&
                              r.zero == ((a & mask) == 0) &&
                              r.skip == (op == 5 ? (a & mask) != 0 : (a & mask) == 0),
                          "ONI/OFFI update Z and preserve HC/CY");
                }
            }
        }
        Fixture f;
        f.state.registers.pc = 0x04c3;
        f.state.registers.a = a;
        check(f.run(21).status == S::Status::ReachedTarget,
              "GTI/MVI/MVI overlay sequence costs21 states");
        check(f.state.registers.a == (a > 128 ? 0 : 127),
              "skipped first MVI does not arm overlay; executed first MVI does");
    }
    {
        Fixture f;
        f.state.interruptEnabled = true;
        f.state.mkh = 4;
        f.state.registers.pc = 0x04c5; // MVI A,7F then overlay-suppressed MVI A,00
        f.state.registers.carry = true;
        f.state.registers.halfCarry = true;
        f.state.registers.zero = true;
        f.state.io.channel = 3;
        f.state.io.statesUntilConversion = 7;
        check(f.run(7).status == S::Status::ReachedTarget &&
                  f.state.pending.kind == S::PendingKind::InterruptEntry &&
                  f.state.pending.savedPsw == 0x59,
              "IRQ saves arithmetic flags and MVI overlay after the first load");
        check(f.run(66).status == S::Status::ReachedTarget && f.state.registers.pc == 0x04c7 &&
                  f.state.registers.l1,
              "real59-state short ADC handler restores interrupted overlay");
        check(f.run(73).status == S::Status::ReachedTarget && f.state.registers.a == 127 &&
                  f.state.registers.carry && f.state.registers.halfCarry && f.state.registers.zero,
              "following MVI remains suppressed across IRQ entry and RETI");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x05a4; // actual MOV A,RXB
        const std::array<S::InputEvent, 1> incoming{{{10, 0x95}}};
        check(f.run(10, incoming).status == S::Status::ReachedTarget,
              "receive/read exact tie completes");
        check(f.state.registers.a == 0x95 && !f.state.rxBufferFull && f.state.fsr &&
                  f.events.count == 2 && f.events.entries[0].kind == S::EventKind::ReceiveReady &&
                  f.events.entries[1].kind == S::EventKind::ReceiveRead,
              "declared peripheral-first tie reads new byte; RXB read does not clear FSR");
    }
    {
        Fixture f;
        const std::array<S::InputEvent, 2> incoming{{{0, 0x90}, {0, 0x91}}};
        const auto result = f.run(0, incoming);
        check(result.status == S::Status::ReceiveOverrun && result.consumedInputs == 1 &&
                  f.state.rxBufferFull && f.state.rxBuffer == 0x90 && f.state.now == 0,
              "unread RXB stops before choosing hardware overwrite semantics");
    }
    for (unsigned pc : {0x026e, 0x0d62}) {
        Fixture f;
        f.state.registers.pc = pc;
        check(f.run(100).status == S::Status::UnsupportedPath && f.state.now == 0 &&
                  f.state.registers.pc == pc && f.events.count == 0,
              "diagnostic/tape frontier executes no invented instruction");
    }
    {
        Fixture f;
        f.config.anmWritePhase = static_cast<Io::AnmWritePhase>(3);
        check(f.run(100).status == S::Status::InvalidState && f.state.now == 0,
              "unknown ADC write scenario rejected before progress");
    }
    {
        Fixture f;
        const std::array<S::InputEvent, 1> incoming{{{0, 0, static_cast<S::InputKind>(2)}}};
        check(f.run(100, incoming).status == S::Status::InvalidState && f.state.now == 0 &&
                  !f.state.fsr,
              "unknown receive event rejected before progress");
    }
    {
        Fixture f;
        f.state.now = f.uart.now = 1;
        check(f.run(0).status == S::Status::InvalidState && f.state.now == 1,
              "time reversal has no side effects");
    }
}
} // namespace schedulerCoreRegression
namespace schedulerBoundaryRegression {
using S = youknow::FirmwareAssignerScheduler;
namespace U = youknow::FirmwareUartTrace;
namespace Io = youknow::FirmwareAssignerIo;
struct Fixture {
    S::State state;
    U::State uart;
    S::Configuration configuration;
    S::Tables tables;
    Io::Inputs inputs;
    S::Events events;
    std::array<U::Event, 32> wire;
    U::EventBuffer output{wire.data(), wire.size(), 0};
    Fixture() { state.mkh = 7; }
    S::Result run(std::uint64_t target) {
        return S::advanceTo(state, uart, configuration, tables, inputs, {}, target, events, output);
    }
};
void run() {
    watch = true;
    {
        Fixture f;
        f.state.io.statesUntilConversion = 0;
        f.inputs.directAdc[0] = 173;
        f.events.count = f.events.entries.size();
        check(f.run(0).status == S::Status::OutputFull, "duezero needs an event slot");
        check(f.state.io.channel == 0 && f.state.io.statesUntilConversion == 0,
              "duezero is retained without partial conversion");
        f.events.count = 0;
        check(f.run(0).status == S::Status::ReachedTarget, "duezero resumes");
        check(f.state.io.channel == 1 && f.state.io.conversion[0] == 173 &&
                  f.state.io.statesUntilConversion == 192 && f.events.count == 1,
              "resumed conversion commits once");
    }
    {
        Fixture f;
        f.state.io.request = true;
        f.state.mkh = 4;
        f.events.count = f.events.entries.size() - 3;
        check(f.run(16).status == S::Status::OutputFull, "IRQ stack reserves all three events");
        check(f.state.registers.sp == 0xffff && f.state.pending.remaining == 0 &&
                  f.state.pending.kind == S::PendingKind::InterruptEntry,
              "entry-capacity failure retains all stack bytes");
        check(f.state.ram[0xfc] == 0 && f.state.ram[0xfd] == 0 && f.state.ram[0xfe] == 0,
              "no partial stack save on full output");
        f.events.count = 0;
        check(f.run(16).status == S::Status::ReachedTarget, "IRQ entry resumes at exact endpoint");
        check(f.state.registers.sp == 0xfffc && f.events.count == 3,
              "all three stack bytes commit once");
        check(f.run(59).status == S::Status::ReachedTarget, "short ADC handler completes");
        check(f.state.foregroundPasses == 0, "RETI to0111 is not a foreground pass");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x0cf5; // actual patch LDAX(HL+)
        f.state.registers.h = 0x20;
        f.state.registers.a = 99;
        check(f.run(7).status == S::Status::UnavailableMemory, "absent patch RAM read diagnoses");
        check(f.state.registers.a == 99 && f.state.registers.l == 0 &&
                  f.state.registers.pc == 0x0cf5 && f.events.count == 0,
              "missing patch read has no fabricated zero or address increment");
        f.state.patchRamAvailable = true;
        f.state.patchRam[0] = 177;
        check(f.run(7).status == S::Status::ReachedTarget, "provided patch read resumes");
        check(f.state.registers.a == 177 && f.state.registers.l == 1,
              "actual supplied patch byte consumed");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x0d2f; // actual patch STAX(HL+)
        f.state.registers.h = 0x20;
        f.state.registers.a = 73;
        check(f.run(7).status == S::Status::UnavailableMemory, "absent patch RAM write diagnoses");
        check(f.state.patchRam[0] == 0 && f.state.registers.l == 0 && f.events.count == 0,
              "missing patch write is atomic");
        f.state.patchRamAvailable = true;
        check(f.run(7).status == S::Status::ReachedTarget, "provided patch write resumes");
        check(f.state.patchRam[0] == 73 && f.state.registers.l == 1 && f.events.count == 1,
              "supplied patch storage receives one physical write");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x07ea;
        f.state.registers.a = 0x95;
        f.output.capacity = 0;
        check(f.run(10).status == S::Status::OutputFull, "TXB completion reserves UART event");
        check(!f.uart.txBufferFull && f.events.count == 0 && f.state.registers.pc == 0x07ea,
              "TXB failure does not commit either CPU or UART write");
        f.output.capacity = f.wire.size();
        check(f.run(10).status == S::Status::ReachedTarget, "TXB resumes atomically");
        check(f.uart.txBufferFull && f.uart.txBuffer == 0x95 && f.events.count == 1 &&
                  f.output.count == 1,
              "one CPU and one UART TXB ledger event");
    }
    {
        Fixture f;
        // The declared UART grid launches at128. An actual MOV PC,A takes
        // ten states, so starting at118 makes its route write coincide with
        // that launch. Four wire slots admit the launch but not the CPU write.
        check(U::writeTxBuffer(f.uart, f.configuration.uart, 0xa6, f.output) == U::Status::Ok,
              "coincident UART fixture queues its byte");
        check(U::advanceTo(f.uart, f.configuration.uart, 118, f.output) == U::Status::Ok,
              "coincident UART fixture reaches instruction start");
        f.state.now = 118;
        f.state.registers.pc = 0x07d7;
        f.state.registers.a = 0xf9;
        f.output.count = 0;
        f.output.capacity = 4;
        check(f.run(128).status == S::Status::OutputFull && f.state.now == 128 &&
                  f.state.pending.remaining == 0 && f.state.registers.pc == 0x07d7 &&
                  f.uart.portC == 0xfd && f.uart.frameOrdinal == 1 &&
                  f.uart.frameStart == 128 && f.output.count == 4 && f.events.count == 0,
              "UART launch precedes and survives a backpressured same-time CPU route write");
        constexpr std::array<U::EventKind, 4> launch{
            U::EventKind::TxBufferEmpty, U::EventKind::FrameBegin,
            U::EventKind::PinLevels, U::EventKind::BitBegin};
        for (std::size_t i = 0; i < launch.size(); ++i)
            check(f.wire[i].kind == launch[i] && f.wire[i].states == 128 &&
                      f.wire[i].frameOrdinal == 1,
                  "launch event order follows the UART transaction before CPU routing");
        check(f.wire[2].pinLevels == 4 && f.wire[3].value == 0,
              "start bit first reaches the previously selected module branch");
        f.output.count = 0;
        f.output.capacity = f.wire.size();
        check(f.run(128).status == S::Status::ReachedTarget && f.output.count == 2 &&
                  f.events.count == 1 && f.uart.portC == 0xf9 && f.uart.frameOrdinal == 1,
              "same-frontier retry commits only the retained CPU write");
        check(f.wire[0].kind == U::EventKind::PortCWrite && f.wire[0].states == 128 &&
                  f.wire[1].kind == U::EventKind::PinLevels && f.wire[1].states == 128 &&
                  f.wire[1].pinLevels == 2 && f.events.entries[0].kind == S::EventKind::PortCWrite,
              "retry emits exact same-time route events without replaying the UART launch");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x0d2f; // actual patch STAX(HL+)
        f.state.registers.h = 0x20;
        f.state.registers.a = 73;
        f.state.patchRamAvailable = true;
        f.state.patchRam.fill(0xa5);
        const auto originalRam = f.state.ram;
        const auto originalPatch = f.state.patchRam;
        f.events.count = f.events.entries.size();
        check(f.run(7).status == S::Status::OutputFull,
              "patch store waits for its complete CPU ledger slot");
        check(f.state.ram == originalRam && f.state.patchRam == originalPatch &&
                  f.state.registers.pc == 0x0d2f && f.state.registers.l == 0 &&
                  f.state.pending.remaining == 0,
              "backpressured patch store retains both memories and address registers");
        f.events.count = 0;
        check(f.run(7).status == S::Status::ReachedTarget && f.events.count == 1 &&
                  f.state.patchRam[0] == 73 && f.state.ram == originalRam &&
                  std::equal(f.state.patchRam.begin() + 1, f.state.patchRam.end(),
                             originalPatch.begin() + 1),
              "resumed sparse patch store changes only its addressed byte once");
        // Supply a coherent warm snapshot at the original patch-load opcode.
        f.state.pending = {};
        f.state.registers.pc = 0x0cf5;
        f.state.registers.l = 0;
        f.state.registers.a = 0;
        check(f.run(14).status == S::Status::ReachedTarget &&
                  f.state.registers.a == 73 && f.state.registers.l == 1,
              "subsequent patch read sees the committed sparse write");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x099d; // PUSH BC
        f.state.registers.b = 0x12;
        f.state.registers.c = 0x34;
        f.state.registers.sp = 0xff01;
        f.state.ram[0] = 0xa5;
        const auto originalRam = f.state.ram;
        check(f.run(13).status == S::Status::InvalidState &&
                  f.state.ram == originalRam && f.state.registers.sp == 0xff01 &&
                  f.state.registers.pc == 0x099d && f.events.count == 0,
              "second-byte stack failure discards the first staged write and SP change");
        f.state.registers.sp = 0xffff;
        check(f.run(13).status == S::Status::ReachedTarget && f.events.count == 2 &&
                  f.state.ram[0xfe] == 0x12 && f.state.ram[0xfd] == 0x34,
              "retried stack transaction commits both bytes in bus order");
        f.state.pending = {};
        f.state.registers.pc = 0x09bf; // POP BC
        f.state.registers.b = f.state.registers.c = 0;
        check(f.run(23).status == S::Status::ReachedTarget &&
                  f.state.registers.b == 0x12 && f.state.registers.c == 0x34 &&
                  f.state.registers.sp == 0xffff,
              "subsequent stack reads observe both committed journal bytes");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x0d2f; // STAX(HL+) at the matrix latch
        f.state.registers.h = 0x1f;
        f.state.registers.l = 0xff;
        f.state.registers.a = 0; // unsupported simultaneous matrix-bank selection
        const auto originalIo = schedulerScenarioRegression::peripheral(f.state.io);
        check(f.run(7).status == S::Status::UnsupportedPath &&
                  f.state.registers.pc == 0x0d2f && f.state.registers.h == 0x1f &&
                  f.state.registers.l == 0xff && f.events.count == 0,
              "unsupported device write rolls back staged address-register changes");
        auto expectedIo = originalIo;
        std::get<6>(expectedIo) -= 7; // elapsed time still reaches instruction completion
        check(schedulerScenarioRegression::peripheral(f.state.io) == expectedIo,
              "unsupported device write preserves peripheral state apart from elapsed time");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x0871;
        f.state.io.statesUntilConversion = 10;
        f.inputs.directAdc[0] = 177;
        check(f.run(10).status == S::Status::ReachedTarget, "ADC/read coincidence completes");
        check(f.state.registers.a == 177 && f.events.entries[0].kind == S::EventKind::AdcConversion,
              "CR read sees peripheral-first conversion");
    }
    {
        Fixture f;
        f.state.registers.pc = 0x055d;
        f.state.io.statesUntilConversion = 14;
        f.inputs.directAdc[0] = 177;
        f.inputs.panelAdc[0] = 19;
        check(f.run(14).status == S::Status::ReachedTarget, "ADC/ANM coincidence completes");
        check(f.state.io.conversion[0] == 177 && f.state.io.anm == 0 && f.state.io.channel == 0 &&
                  f.state.io.statesUntilConversion == 192,
              "old bank samples before ANM changes and restarts");
        check(f.events.count == 2 && f.events.entries[0].kind == S::EventKind::AdcConversion &&
                  f.events.entries[1].kind == S::EventKind::AnmWrite,
              "tie ledger ordering");
    }
    watch = false;
    check(allocations == 0, "all scheduler boundaries allocate no heap memory");
}
} // namespace schedulerBoundaryRegression
int main() {
    schedulerScenarioRegression::run();
    schedulerCoreRegression::run(check);
    schedulerBoundaryRegression::run();
    std::printf("PASS %u scheduler regression assertions\n", assertions);
    return 0;
}
