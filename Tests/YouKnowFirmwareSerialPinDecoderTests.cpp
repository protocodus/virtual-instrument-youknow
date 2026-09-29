#include "../Source/DSP/YouKnowFirmwareSerialPinDecoder.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <tuple>
#include <vector>

namespace D = youknow::FirmwareSerialPinDecoder;
namespace {
std::size_t assertions = 0;
bool prohibitAllocation = false;

void check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool same(const D::State& a, const D::State& b) {
    return std::tie(a.now, a.moduleLevel, a.instantClosed, a.active, a.frameStart,
                    a.nextSample, a.sampleIndex, a.byte, a.pendingStart)
        == std::tie(b.now, b.moduleLevel, b.instantClosed, b.active, b.frameStart,
                    b.nextSample, b.sampleIndex, b.byte, b.pendingStart);
}

// Independent transmitter fixture: literals below are protocol constants, not
// imported candidate fields. It provides pin levels only, including redundant
// levels, and never passes the expected byte to the decoder.
std::vector<D::PinLevel> frame(std::uint8_t byte, std::uint64_t start,
                               bool moduleRoute = true) {
    std::vector<D::PinLevel> pins;
    pins.push_back({start, !moduleRoute});
    for (unsigned bit = 0; bit != 8; ++bit)
        pins.push_back({start + 128 * (bit + 1), !moduleRoute || ((byte >> bit) & 1)});
    pins.push_back({start + 1152, true});
    return pins;
}

void fullValuePhaseRouteSweep() {
    for (unsigned byte = 0; byte != 256; ++byte) {
        for (unsigned phase = 0; phase != 128; ++phase) {
            for (const bool routed : {false, true}) {
                auto pins = frame(static_cast<std::uint8_t>(byte), phase, routed);
                D::State state;
                std::array<D::ByteReady, 2> bytes{};
                D::Output output{bytes};
                for (const auto pin : pins) {
                    check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
                          "every byte/phase/route pin is accepted");
                    check(D::valid(state), "state stays valid after every pin");
                }
                check(D::advanceTo(state, phase + 1280, D::Boundary::Through, output).status
                          == D::Status::ReachedTarget, "frame end is accepted");
                check(output.count == static_cast<std::size_t>(routed),
                      "inactive route cannot leak sender byte metadata");
                if (routed) {
                    check(bytes[0].value == byte && bytes[0].states == phase + 1280,
                          "all 256 bytes decode at independent frame-end coordinate");
                }
            }
        }
    }
}

void partitionsAndBackpressure() {
    std::vector<D::PinLevel> pins;
    constexpr std::array<std::uint8_t, 5> expected{0x00, 0xff, 0x55, 0xaa, 0x88};
    for (std::size_t index = 0; index != expected.size(); ++index) {
        auto next = frame(expected[index], 7 + 1280 * index);
        pins.insert(pins.end(), next.begin(), next.end());
    }
    for (const std::uint64_t stride : {1, 7, 63, 64, 127, 128, 191, 255, 1280, 9000}) {
        D::State state;
        std::array<D::ByteReady, 8> bytes{};
        D::Output output{bytes};
        std::size_t pinIndex = 0;
        for (std::uint64_t target = 0;; target = std::min<std::uint64_t>(6410, target + stride)) {
            while (pinIndex < pins.size() && pins[pinIndex].states <= target) {
                check(D::applyPin(state, pins[pinIndex++], output).status
                          == D::Status::ReachedTarget, "partition pin consumed once");
            }
            check(D::advanceTo(state, target, D::Boundary::Through, output).status
                      == D::Status::ReachedTarget, "arbitrary partition closes target");
            if (target == 6410)
                break;
        }
        check(output.count == expected.size(), "partitions retain every contiguous frame");
        for (std::size_t index = 0; index != output.count; ++index)
            check(bytes[index].value == expected[index] && bytes[index].states == 1287 + 1280 * index,
                  "partitioned byte and ready timestamp match independent source");
    }

    D::State state;
    std::array<D::ByteReady, 1> storage{};
    D::Output output{storage};
    for (const auto pin : frame(0x55, 0))
        check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
              "backpressure fixture first frame");
    // Start next frame before closing old frame's ready instant; capacity zero
    // forces retry with the new start edge already retained, not consumed twice.
    D::Output none{};
    check(D::applyPin(state, {1280, false}, none).status == D::Status::ReachedTarget,
          "same-time next start edge applied before old byte delivery");
    check(D::advanceTo(state, 1280, D::Boundary::Through, none).status == D::Status::OutputFull,
          "frame end backpressure detected");
    check(state.pendingStart && D::valid(state), "pending next start is resumable");
    check(D::advanceTo(state, 1280, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && output.count == 1 && storage[0].value == 0x55,
          "drain resumes exact prior byte once");
    output.count = 0;
    auto next = frame(0xa6, 1280);
    for (std::size_t index = 1; index != next.size(); ++index)
        check(D::applyPin(state, next[index], output).status == D::Status::ReachedTarget,
              "new frame survived output-full boundary");
    check(D::advanceTo(state, 2560, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && output.count == 1 && storage[0].value == 0xa6,
          "contiguous frame decodes after output drain");

    // A pin after pending byte delivery is not consumed when output is full.
    const auto saved = state;
    check(D::applyPin(state, {2700, false}, output).status == D::Status::ReachedTarget,
          "third start fixture");
    for (const auto pin : frame(0xff, 2700)) {
        if (pin.states > 2700)
            check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
                  "third frame pins with output already full");
    }
    check(D::applyPin(state, {4000, false}, output).status == D::Status::OutputFull
              && state.now == 3980 && state.moduleLevel,
          "future pin is unapplied under output backpressure");
    output.count = 0;
    check(D::applyPin(state, {4000, false}, output).status == D::Status::ReachedTarget
              && output.count == 1 && storage[0].value == 0xff && !state.moduleLevel,
          "same future pin retry delivers prior byte and applies once");
    state = D::State{};
    check(D::valid(state) && !same(state, saved), "explicit reset clears partial history");
}

void boundariesAndErrors() {
    std::array<D::ByteReady, 4> bytes{};
    D::Output output{bytes};
    D::State state;
    check(D::applyPin(state, {0, false}, output).status == D::Status::ReachedTarget,
          "start at initial open instant");
    check(D::advanceTo(state, 64, D::Boundary::Before, output).status == D::Status::ReachedTarget,
          "Before defers same-time sampling");
    check(D::applyPin(state, {64, true}, output).status == D::Status::ReachedTarget,
          "pin edge precedes exact start-center sample");
    const auto saved = state;
    check(D::advanceTo(state, 64, D::Boundary::Through, output).status == D::Status::InvalidStart
              && same(state, saved) && output.count == 0,
          "invalid start sample rejects call atomically");

    state = {};
    for (const auto pin : frame(0x31, 0)) {
        if (pin.states < 1152)
            check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
                  "bad-stop fixture");
    }
    check(D::applyPin(state, {1152, false}, output).status == D::Status::ReachedTarget,
          "bad stop is pin data before sample");
    const auto badStop = state;
    check(D::advanceTo(state, 1280, D::Boundary::Through, output).status == D::Status::FramingError
              && same(state, badStop) && output.count == 0,
          "low stop center rejects without publishing byte");

    state = {};
    check(D::applyPin(state, {0, false}, output).status == D::Status::ReachedTarget,
          "center-tie start fixture");
    check(D::applyPin(state, {192, true}, output).status == D::Status::ReachedTarget,
          "rising edge exactly at first data center");
    check(D::applyPin(state, {192, false}, output).status == D::Status::ReachedTarget
              && D::applyPin(state, {192, true}, output).status == D::Status::ReachedTarget,
          "all ordered same-time pin changes precede sampling");
    check(D::advanceTo(state, 192, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && state.byte == 1,
          "first center samples final level at that instant");
    check(D::applyPin(state, {320, false}, output).status == D::Status::ReachedTarget,
          "falling edge exactly at second data center");
    check(D::applyPin(state, {1216, true}, output).status == D::Status::ReachedTarget,
          "rising edge exactly at stop center");
    check(D::advanceTo(state, 1216, D::Boundary::Through, output).status
              == D::Status::ReachedTarget, "stop center samples new high level");
    const auto shortStop = state;
    check(D::applyPin(state, {1250, false}, output).status == D::Status::FramingError
              && same(state, shortStop) && output.count == 0,
          "early next start is rejected atomically for this fixed full-stop model");
    check(D::advanceTo(state, 1280, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && bytes[0].value == 1,
          "center-tie bits yield independent expected byte");

    state = {};
    output.count = 0;
    for (const auto pin : frame(0x77, 0))
        check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
              "multi-frame atomic failure fixture");
    check(D::applyPin(state, {1280, false}, output).status == D::Status::ReachedTarget,
          "pending contiguous start fixture");
    const auto beforeBatch = state;
    check(D::advanceTo(state, 2560, D::Boundary::Through, output).status == D::Status::FramingError
              && same(state, beforeBatch) && output.count == 0,
          "malformed second frame rolls back prior byte appended by same call");

    state = {};
    check(D::advanceTo(state, 17, D::Boundary::Through, output).status == D::Status::ReachedTarget,
          "idle target closes instant");
    const auto closed = state;
    check(D::applyPin(state, {17, false}, output).status == D::Status::TimeReversal
              && same(state, closed), "late same-time pin cannot change a sampled instant");
    check(D::advanceTo(state, 17, D::Boundary::Before, output).status == D::Status::ReachedTarget
              && same(state, closed), "Before cannot reopen closed target");
    check(D::advanceTo(state, 16, D::Boundary::Through, output).status == D::Status::TimeReversal
              && same(state, closed), "time reversal atomic");
    check(D::advanceTo(state, 20, static_cast<D::Boundary>(255), output).status
              == D::Status::InvalidArgument && same(state, closed), "invalid enum atomic");
    check(D::advanceTo(state, D::maximumTime + 1, D::Boundary::Through, output).status
              == D::Status::InvalidArgument && same(state, closed), "overflow target atomic");
    check(D::applyPin(state, {D::maximumTime, false}, output).status
              == D::Status::InvalidArgument && same(state, closed), "frame-end overflow atomic");
    output.count = output.bytes.size() + 1;
    check(D::advanceTo(state, 20, D::Boundary::Through, output).status
              == D::Status::InvalidArgument && same(state, closed), "invalid output count atomic");
    output.count = 0;
    state.active = true;
    const auto invalid = state;
    check(D::advanceTo(state, 20, D::Boundary::Through, output).status
              == D::Status::InvalidState && same(state, invalid), "invalid state atomic");

    state = {};
    for (const auto pin : frame(0x8e, D::maximumTime - 1280))
        check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
              "last representable full frame accepts every pin");
    check(D::advanceTo(state, D::maximumTime, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && bytes[0].value == 0x8e && D::valid(state),
          "last full frame completes without timestamp overflow");
}

void noAllocation() {
    const auto pins = frame(0x88, 5);
    D::State state;
    std::array<D::ByteReady, 1> bytes{};
    D::Output output{bytes};
    prohibitAllocation = true;
    for (const auto pin : pins)
        check(D::applyPin(state, pin, output).status == D::Status::ReachedTarget,
              "allocation-free edge consumption");
    check(D::advanceTo(state, 1285, D::Boundary::Through, output).status
              == D::Status::ReachedTarget && bytes[0].value == 0x88,
          "allocation-free delivery");
    prohibitAllocation = false;
}
} // namespace

void* operator new(std::size_t size) {
    if (prohibitAllocation)
        std::abort();
    if (void* result = std::malloc(size))
        return result;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

int main() {
    fullValuePhaseRouteSweep();
    partitionsAndBackpressure();
    boundariesAndErrors();
    noAllocation();
    std::cout << "PASS " << assertions << " assertions\n";
}
