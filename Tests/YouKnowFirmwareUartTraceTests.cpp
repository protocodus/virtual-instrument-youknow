// NEC Stock500375 (April1987), printed7-3/7-4: TXB is distinct from the
// shifter, FST means buffer empty, full TXB writes overwrite, TxE drains active
// shift but retains pending TXB. Roland service pp6,11 supplies PC2 routing.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
// https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf
// Independent primary-semantics UART tests. Frame bits are closed-form8N1;
// no candidate transition helper or MAME UART is used as expected output.
// Idle baud-grid and peripheral-first ties are declared scenarios, not measured
// original-7810 launch/RXB phases. StopCenter is an observation, not RXB-ready.
#include "../Source/DSP/YouKnowFirmwareUartTrace.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <tuple>
#include <vector>
static bool allocationsOn = false;
static std::size_t allocations = 0;
void *operator new(std::size_t n) {
    if (allocationsOn)
        ++allocations;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
namespace U = youknow::FirmwareUartTrace;
using K = U::EventKind;
using S = U::Status;
unsigned checks = 0, failures = 0;
void check(bool ok, const char *m) {
    ++checks;
    if (!ok && failures++ < 20)
        std::cerr << m << '\n';
}
auto state(const U::State &s) {
    return std::tie(s.now, s.transmitEnabled, s.txBufferFull, s.txBuffer, s.fst, s.frameActive,
                    s.frameByte, s.bitIndex, s.frameOrdinal, s.frameStart, s.nextBit, s.idleLaunch,
                    s.stopCenterObserved, s.portC, s.txd, s.moduleRxD, s.midiLogic);
}
auto event(const U::Event &e) {
    return std::tie(e.states, e.frameOrdinal, e.kind, e.value, e.bitIndex, e.pinLevels);
}
struct Buffer {
    std::array<U::Event, 32> data{};
    U::EventBuffer b{data.data(), data.size(), 0};
    void clear() { b.count = 0; }
};
bool bit(unsigned v, unsigned i) {
    return i == 0 ? false : i == 9 ? true : bool(v & (1u << (i - 1)));
}
void pins(const U::State &s, bool tx, bool module) {
    check(s.txd == tx && s.moduleRxD == (module ? tx : true) && s.midiLogic == (module ? true : tx),
          "physical OR gate pin levels follow current bit and route");
}
void bytes() {
    for (unsigned phase = 0; phase < 128; ++phase)
        for (unsigned v = 0; v < 256; ++v)
            for (unsigned route = 0; route < 2; ++route) {
                U::State s;
                U::Configuration c{std::uint8_t(phase)};
                Buffer b;
                s.portC = route ? 0xfd : 0xf9;
                check(U::testAndClearFst(s), "explicit warm empty FST initially set");
                check(U::writeTxBuffer(s, c, std::uint8_t(v), b.b) == S::Ok && s.txBufferFull &&
                          !s.fst,
                      "TXB fill is distinct from shifter/FST");
                b.clear();
                const std::uint64_t t = phase ? phase : 128;
                check(U::advanceTo(s, c, t - 1, b.b) == S::Ok && !s.frameActive,
                      "idle launch is strictly future configured edge");
                pins(s, true, route);
                b.clear();
                for (unsigned i = 0; i < 10; ++i) {
                    check(U::advanceTo(s, c, t + 128 * i, b.b) == S::Ok,
                          "frame bit edge supported");
                    check(s.frameActive && s.frameStart == t && s.bitIndex == i && s.frameByte == v,
                          "closed-form byte bit position");
                    pins(s, bit(v, i), route);
                    check(!s.txBufferFull && s.fst, "buffer-empty flag while frame still active");
                    b.clear();
                    check(U::advanceTo(s, c, t + 128 * i + 63, b.b) == S::Ok,
                          "bit midpoint advance");
                    pins(s, bit(v, i), route);
                    b.clear();
                }
                check(U::advanceTo(s, c, t + 1216, b.b) == S::Ok && s.frameActive &&
                          s.stopCenterObserved,
                      "stop-center observation precedes frame completion");
                check(std::count_if(b.data.begin(), b.data.begin() + b.b.count,
                                    [](auto &e) { return e.kind == K::StopCenter; }) == 1,
                      "one stop-center event");
                b.clear();
                check(U::advanceTo(s, c, t + 1280, b.b) == S::Ok && !s.frameActive &&
                          s.frameOrdinal == 1,
                      "exact ten-bit frame occupancy");
                pins(s, true, route);
                check(U::nextEventTime(s) == U::noEvent,
                      "empty transmitter has no pending wire event");
            }
}
void pipeline() {
    U::State s;
    U::Configuration c;
    Buffer b;
    U::testAndClearFst(s);
    check(U::writeTxBuffer(s, c, 0x11, b.b) == S::Ok, "first buffer write");
    b.clear();
    check(U::advanceTo(s, c, 128, b.b) == S::Ok && s.fst, "first launch empties buffer");
    U::testAndClearFst(s);
    b.clear();
    check(U::advanceTo(s, c, 200, b.b) == S::Ok, "within-frame time");
    check(U::writeTxBuffer(s, c, 0x22, b.b) == S::Ok && s.txBufferFull && !s.fst &&
              s.frameByte == 0x11,
          "one queued byte independent of current shift");
    b.clear();
    check(U::advanceTo(s, c, 300, b.b) == S::Ok, "overwrite time");
    check(U::writeTxBuffer(s, c, 0x33, b.b) == S::Ok && s.txBuffer == 0x33 && s.frameByte == 0x11,
          "full TXB write overwrites only pending byte");
    check(std::count_if(b.data.begin(), b.data.begin() + b.b.count,
                        [](auto &e) { return e.kind == K::TxBufferOverwrite; }) == 1,
          "overwrite reported");
    b.clear();
    check(U::advanceTo(s, c, 1408, b.b) == S::Ok && s.frameByte == 0x33 && s.frameStart == 1408 &&
              s.frameOrdinal == 2 && !s.txBufferFull && s.fst,
          "buffered continuation has no stop-to-start gap");
    U::testAndClearFst(s);
    b.clear();
    check(U::writeTxBuffer(s, c, 0x44, b.b) == S::Ok && s.txBufferFull && s.frameByte == 0x33,
          "CPU write at frame-end follows peripheral transfer");
    b.clear();
    check(U::advanceTo(s, c, 2688, b.b) == S::Ok && s.frameByte == 0x44 && s.frameStart == 2688 &&
              s.frameOrdinal == 3,
          "third frame consumes CPU tie write once");
}
void enableAndRoute() {
    U::State s;
    U::Configuration c;
    Buffer b;
    U::testAndClearFst(s);
    U::writeTxBuffer(s, c, 0, b.b);
    b.clear();
    U::advanceTo(s, c, 128, b.b);
    U::testAndClearFst(s);
    b.clear();
    U::advanceTo(s, c, 200, b.b);
    U::writeTxBuffer(s, c, 0xa5, b.b);
    b.clear();
    check(U::setTransmitEnabled(s, c, false, b.b) == S::Ok && s.frameActive && s.txBufferFull,
          "TxE clear retains active and queued bytes");
    b.clear();
    check(U::writePortC(s, 0xf9, b.b) == S::Ok, "midframe routing diagnostic");
    pins(s, false, false);
    check(s.frameStart == 128 && s.frameByte == 0, "PC route write does not restart UART");
    b.clear();
    U::advanceTo(s, c, 333, b.b);
    check(U::writePortC(s, 0xfd, b.b) == S::Ok, "restore route within data bit");
    pins(s, false, true);
    b.clear();
    check(U::advanceTo(s, c, 2000, b.b) == S::Ok && !s.frameActive && s.txBufferFull && !s.fst &&
              s.txBuffer == 0xa5,
          "TxE drains only active frame and preserves queued TXB");
    pins(s, true, true);
    b.clear();
    check(U::setTransmitEnabled(s, c, true, b.b) == S::Ok && !s.frameActive && !s.fst,
          "reenabling does not invent immediate transfer");
    b.clear();
    check(U::advanceTo(s, c, 2048, b.b) == S::Ok && s.frameActive && s.frameByte == 0xa5 &&
              !s.txBufferFull && s.fst,
          "held byte launches at explicit future idle edge");
}
struct Result {
    U::State s;
    std::vector<U::Event> events;
};
Result chunked(unsigned step, unsigned capacity, unsigned initialFill) {
    Result r;
    U::Configuration c{37};
    std::array<U::Event, 32> storage{};
    U::EventBuffer b{storage.data(), capacity, 0};
    U::testAndClearFst(r.s);
    U::writeTxBuffer(r.s, c, 0xa6, b);
    r.events.insert(r.events.end(), storage.begin(), storage.begin() + b.count);
    b.count = initialFill;
    bool fake = true;
    for (std::uint64_t target = std::min(step, 2000u);;
         target = std::min<std::uint64_t>(2000, target + step)) {
        for (unsigned guard = 0; guard < 1000; ++guard) {
            auto result = U::advanceTo(r.s, c, target, b);
            check(result == S::Ok || result == S::OutputFull,
                  "chunked UART remains valid after event drain");
            if (!fake)
                r.events.insert(r.events.end(), storage.begin(), storage.begin() + b.count);
            else {
                r.events.insert(r.events.end(), storage.begin() + initialFill,
                                storage.begin() + b.count);
                fake = false;
            }
            b.count = 0;
            if (result == S::Ok)
                break;
            if (guard == 999)
                check(false, "chunked UART makes bounded progress");
        }
        if (target == 2000)
            break;
    }
    return r;
}
void chunksAndBuffers() {
    auto full = chunked(2000, 32, 0);
    for (unsigned step : {1, 7, 16, 83, 127, 128, 129, 511, 2000})
        for (unsigned used = 0; used <= 8; ++used) {
            auto r = chunked(step, 8, used);
            check(state(r.s) == state(full.s),
                  "complete UART state independent of chunk/drain schedule");
            check(r.events.size() == full.events.size(),
                  "complete event count independent of chunk/drain schedule");
            if (r.events.size() == full.events.size())
                for (unsigned i = 0; i < r.events.size(); ++i)
                    check(event(r.events[i]) == event(full.events[i]),
                          "every peripheral event exact under chunk/drain");
        }
    U::State s;
    U::Configuration c;
    Buffer b;
    U::writeTxBuffer(s, c, 0x22, b.b);
    b.b.count = b.b.capacity;
    auto before = s;
    check(U::writeTxBuffer(s, c, 0x44, b.b) == S::OutputFull && state(s) == state(before),
          "full-buffer overwrite transaction is atomic");
    check(U::writePortC(s, 0xf9, b.b) == S::OutputFull && state(s) == state(before),
          "full-buffer PC transaction is atomic");
    check(U::setTransmitEnabled(s, c, false, b.b) == S::OutputFull && state(s) == state(before),
          "full-buffer enable transaction is atomic");
    b.clear();
    check(U::writeTxBuffer(s, c, 0x44, b.b) == S::Ok && s.txBuffer == 0x44,
          "retry overwrites once after drain");
}
void boundaries() {
    for (unsigned t : {0, 1, 127, 128, 129, 255, 256}) {
        U::State s;
        s.now = t;
        U::Configuration c;
        Buffer b;
        U::writeTxBuffer(s, c, 0xff, b.b);
        const auto expected = (std::uint64_t(t) / 128 + 1) * 128;
        check(s.idleLaunch == expected, "write at exact grid edge waits strictly future edge");
    }
    U::State s;
    U::Configuration c;
    Buffer b;
    auto before = s;
    check(U::advanceTo(s, U::Configuration{128}, 100, b.b) == S::InvalidState &&
              state(s) == state(before),
          "bad phase rejected without mutation");
    s.now = 100;
    before = s;
    check(U::advanceTo(s, c, 99, b.b) == S::TimeReversal && state(s) == state(before),
          "time reversal rejected without mutation");
    s = {};
    allocations = 0;
    allocationsOn = true;
    for (unsigned i = 0; i < 100; ++i) {
        b.clear();
        U::writeTxBuffer(s, c, std::uint8_t(i), b.b);
        b.clear();
        U::advanceTo(s, c, s.now + 1536, b.b);
    }
    allocationsOn = false;
    check(allocations == 0, "actual UART processing performs no allocations");
}
void finalInvalidStateGuards() {
    U::Configuration c;
    Buffer b;
    U::State late;
    late.now = U::maximumTime - U::frameStates - U::bitStates - 1;
    check(U::writeTxBuffer(late, c, 0xff, b.b) == S::Ok,
          "largest completed-frame region accepts buffer");
    b.clear();
    check(U::advanceTo(late, c, U::maximumTime, b.b) == S::Ok && !late.frameActive,
          "valid near-limit frame completes");
    b.clear();
    auto before = late;
    check(U::writeTxBuffer(late, c, 0x55, b.b) == S::InvalidState && state(late) == state(before) &&
              b.b.count == 0,
          "too-late buffer rejected atomically");
    check(U::advanceTo(late, c, U::noEvent, b.b) == S::InvalidState && state(late) == state(before),
          "oversized target rejected atomically");
    U::State running;
    b.clear();
    U::writeTxBuffer(running, c, 0, b.b);
    b.clear();
    U::advanceTo(running, c, 128 + 9 * 128 + 64, b.b);
    b.clear();
    for (unsigned variant = 0; variant < 4; ++variant) {
        auto bad = running;
        if (variant == 0)
            bad.stopCenterObserved = false;
        if (variant == 1) {
            bad.now = 128;
            bad.stopCenterObserved = false;
        }
        if (variant == 2)
            --bad.now;
        if (variant == 3)
            bad.frameOrdinal = U::noEvent;
        auto saved = bad;
        check(U::advanceTo(bad, c, bad.now, b.b) == S::InvalidState && state(bad) == state(saved) &&
                  b.b.count == 0,
              "inconsistent active-frame state rejected atomically");
    }
    U::State ordinal;
    ordinal.frameOrdinal = U::noEvent - 1;
    check(U::writeTxBuffer(ordinal, c, 1, b.b) == S::Ok, "last ordinal pending buffer accepted");
    b.clear();
    before = ordinal;
    check(U::advanceTo(ordinal, c, 128, b.b) == S::InvalidState &&
              state(ordinal) == state(before) && b.b.count == 0,
          "ordinal sentinel cannot be reached by partial mutation");
}
int main() {
    finalInvalidStateGuards();
    bytes();
    pipeline();
    enableAndRoute();
    chunksAndBuffers();
    boundaries();
    std::cout << "{\"assertions\":" << checks << ",\"failures\":" << failures
              << ",\"byte_phase_route_cases\":65536}\n";
    return failures ? 1 : 0;
}
