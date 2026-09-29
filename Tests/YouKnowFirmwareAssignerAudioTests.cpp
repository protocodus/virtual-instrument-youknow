// Original A-5 sender -> actual module pin bits -> declared relative RXB schedule ->
// actual product audio bridge. No A-5 foreground duration or RXB latch latency
// is inferred. Each isolated original 0837 call is a supplied procedure snapshot.
// Hardware-only tail drain is allowed only after empty FIFO and both real TXB
// writes are proved at AwaitingForeground. No further inputs/writes are supplied.
#include "../Source/DSP/YouKnowEngine.h"
#include "../Source/DSP/YouKnowFirmwareAssignerTrace.h"
#include "../Source/DSP/YouKnowProductFidelity.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

using namespace youknow;
namespace U = FirmwareUartTrace;
using A = FirmwareAssignerTrace;
using S = FirmwareSerialTrace;
using Engine = YouKnowEngine;
namespace youknow {
struct YouKnowTestAccess {
    static const auto &tables(const Engine &engine) {
        return engine.firmwareSerialParameterTables_;
    }
    static const auto &voice(const Engine &engine) { return engine.voices_[0]; }
};
} // namespace youknow

constexpr unsigned sampleRate = 48000;
// Independent protocol oracle, not imported from the UART under test. Original
// NEC SML4E/SMH0E at 12 MHz gives 8N1/31,250 baud, 128 CPU states/bit.
// Original NEC manual printed7-4/6; receiver latch subcycle remains unspecified.
// https://drive.google.com/file/d/0B44NKm9yPA1bNDFXZnFrdG1PdDA/view
constexpr std::uint64_t receiverBitStates = 128;
constexpr std::uint64_t receiverFrameStates = 1280;
std::uint64_t assertions = 0;
void check(bool condition, const char *message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}
struct Decoded {
    std::uint64_t start;
    std::uint8_t value;
};
struct Burst {
    std::vector<U::Event> wire;
    std::vector<Decoded> decoded;
    std::uint64_t callerReturn = 0;
    unsigned txWrites = 0;
};

// Independent receiver-side decoding uses only pin edge times and levels. It
// never reads FrameBegin/FrameEnd/BitBegin metadata, frameByte or event.value.
std::vector<Decoded> decode(const std::vector<U::Event> &events) {
    std::vector<std::pair<std::uint64_t, bool>> edges;
    bool previous = true;
    for (const auto &event : events) {
        if (event.kind != U::EventKind::PinLevels)
            continue;
        const bool value = (event.pinLevels & 2) != 0;
        if (value != previous)
            edges.emplace_back(event.states, value);
        previous = value;
    }
    const auto levelAt = [&](std::uint64_t time) {
        bool level = true;
        for (auto [edge, value] : edges) {
            if (edge > time)
                break;
            level = value;
        }
        return level;
    };
    std::vector<Decoded> result;
    std::uint64_t nextStart = 0;
    for (auto [time, value] : edges) {
        if (value || time < nextStart)
            continue;
        check(!levelAt(time + receiverBitStates / 2),
              "module start stays low at the independent half-bit sample");
        unsigned byte = 0;
        for (unsigned bit = 0; bit < 8; ++bit)
            byte |= unsigned(levelAt(time + (bit + 1) * receiverBitStates + receiverBitStates / 2))
                    << bit;
        check(levelAt(time + 9 * receiverBitStates + receiverBitStates / 2),
              "module stop is high at the independent stop-center sample");
        result.push_back({time, static_cast<std::uint8_t>(byte)});
        nextStart = time + receiverFrameStates;
    }
    return result;
}

Burst burst(unsigned phase, unsigned initialRoute, unsigned command, unsigned value) {
    A::State cpu;
    cpu.registers.pc = 0x0837;
    cpu.registers.a = value;
    cpu.registers.b = command;
    cpu.ram[0xfd] = 0x34;
    cpu.ram[0xfe] = 0x12; // explicit unsupported caller return1234
    U::State uart;
    uart.portC = static_cast<std::uint8_t>(initialRoute);
    U::Configuration configuration{static_cast<std::uint8_t>(phase)};
    Burst result;
    for (unsigned guard = 0; guard < 100; ++guard) {
        A::Events cpuEvents;
        std::array<U::Event, 256> storage{};
        U::EventBuffer wire{storage.data(), storage.size(), 0};
        const auto step = A::advanceTo(cpu, uart, configuration, {}, 100000, cpuEvents, wire);
        result.wire.insert(result.wire.end(), storage.begin(), storage.begin() + wire.count);
        for (std::size_t i = 0; i < cpuEvents.count; ++i)
            result.txWrites += cpuEvents.entries[i].kind == A::EventKind::TxBufferWrite;
        if (step.status == A::Status::OutputFull)
            continue;
        check(step.status == A::Status::AwaitingForeground,
              "original 0837 returns at an explicit unmodeled foreground boundary");
        check(cpu.ram[0xc1] == cpu.ram[0xc2],
              "sender FIFO is empty before hardware-only tail drain");
        check(result.txWrites == 2, "original 0837 writes command and value, never FD, to TXB");
        check(uart.frameOrdinal + unsigned(uart.txBufferFull) == 2,
              "both bytes reach the transmitter before foreground return");
        result.callerReturn = cpu.now;
        break;
    }
    check(result.callerReturn != 0, "bounded sender call reaches its foreground return");
    // Only UART remains: declared no further CPU inputs, writes or producers.
    std::array<U::Event, 256> storage{};
    U::EventBuffer wire{storage.data(), storage.size(), 0};
    check(U::advanceTo(uart, configuration, uart.now + 4096, wire) == U::Status::Ok,
          "autonomous UART tail drains under the no-future-write scenario");
    check(!uart.frameActive && !uart.txBufferFull,
          "tail drain completes the shifter and queued buffer");
    result.wire.insert(result.wire.end(), storage.begin(), storage.begin() + wire.count);
    result.decoded = decode(result.wire);
    check(result.decoded.size() == 2, "module pin decoder sees exactly two complete frames");
    check(result.decoded[0].value == command && result.decoded[1].value == value,
          "module pin bits decode the supplied command and value");
    check(result.decoded[1].start - result.decoded[0].start == receiverFrameStates,
          "buffered frames retain the independent 1280-state protocol spacing");
    return result;
}

std::vector<S::ByteReady> schedule(const Burst &note, const Burst &cutoff, bool parameter) {
    std::vector<S::ByteReady> result;
    // Supplied receive origins, not inferred physical TXB/RXB latency. Preserve
    // observed within-burst spacing from actual module pin decoding.
    for (const auto &byte : note.decoded)
        result.push_back({5000 + byte.start - note.decoded[0].start, byte.value});
    if (parameter)
        for (const auto &byte : cutoff.decoded)
            result.push_back({205000 + byte.start - cutoff.decoded[0].start, byte.value});
    return result;
}

std::unique_ptr<Engine> engine(const std::vector<S::ByteReady> &bytes, unsigned factor) {
    auto result = std::make_unique<Engine>();
    ProductFidelityProfile::configureBeforePrepare(*result);
    EngineParameters p;
    p.calibration = p.aging = p.chorusNoise = 0;
    p.sawEnabled = true;
    p.pulseEnabled = true;
    p.subLevel = .2f;
    p.attack = 0;
    p.decay = .1f;
    p.release = .1f;
    p.cutoff = .75f;
    p.velocityDepth = 0;
    p.chorus = ChorusMode::One;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    ProductFidelityProfile::applyTo(p);
    result->setParameters(p);
    Engine::FirmwareSerialReplayConfiguration replay;
    replay.schedule = bytes;
    check(result->configureFirmwareSerialReplay(replay),
          "actual audio engine accepts the declared RXB-ready schedule");
    result->prepare(sampleRate, 128, int(factor));
    return result;
}

int main() {
    std::uint64_t minReturn = U::noEvent, maxReturn = 0;
    for (unsigned phase = 0; phase < 128; ++phase)
        for (unsigned route : {0xfd, 0xf9}) {
            const auto result = burst(phase, route, 0x88, 48);
            minReturn = std::min(minReturn, result.callerReturn);
            maxReturn = std::max(maxReturn, result.callerReturn);
        }
    const auto note = burst(37, 0xf9, 0x88, 48);
    const auto cutoff = burst(91, 0xfd, 0x95, 20);
    const auto bytes = schedule(note, cutoff, true), controlBytes = schedule(note, cutoff, false);
    std::cout << "{\"phase_route_scenes\":256,\"caller_return_min\":" << minReturn
              << ",\"caller_return_max\":" << maxReturn << ",\"audio\":[";
    for (unsigned factor : {1, 4}) {
        auto live = engine(bytes, factor), control = engine(controlBytes, factor);
        auto block = engine(bytes, factor);
        auto oracle = live->firmwareSerialState();
        const auto tables = YouKnowTestAccess::tables(*live);
        constexpr unsigned count = sampleRate * 18 / 100;
        std::vector<float> left(count), right(count), refLeft(count), refRight(count), bl(count),
            br(count);
        double energy = 0, difference = 0;
        unsigned exactPrefix = 0;
        for (unsigned i = 0; i < count; ++i) {
            live->process(&left[i], &right[i], 1);
            control->process(&refLeft[i], &refRight[i], 1);
            check(std::isfinite(left[i]) && std::isfinite(right[i]),
                  "actual product audio remains finite");
            energy += double(left[i]) * left[i] + double(right[i]) * right[i];
            difference += std::pow(double(left[i]) - refLeft[i], 2) +
                          std::pow(double(right[i]) - refRight[i], 2);
            if ((i + 1) * 4000000ull <= 205000ull * sampleRate) {
                check(left[i] == refLeft[i] && right[i] == refRight[i],
                      "audio before the supplied parameter-ready origin remains bit exact");
                ++exactPrefix;
            }
        }
        for (unsigned offset = 0; offset < count;) {
            const unsigned n = std::min(83u, count - offset);
            block->process(bl.data() + offset, br.data() + offset, int(n));
            offset += n;
        }
        check(left == bl && right == br,
              "sender-derived audio is invariant between blocks 1 and 83");
        check(energy > 1e-8 && difference > 1e-8,
              "note audio and later cutoff change both have nonzero energy");
        check(live->firmwareSerialStatus() == S::Status::ReachedTarget,
              "engine services the complete derived receive schedule");
        std::size_t cursor = 0;
        unsigned pitWrites = 0, converters = 0, reads = 0;
        Engine::FirmwareSerialReplayConfiguration replay;
        replay.schedule = bytes;
        for (unsigned guard = 0; guard < 1000; ++guard) {
            S::Events events;
            const auto step = S::advanceTo(oracle, tables, replay.inputs, replay.cpu,
                                           std::span<const S::ByteReady>(bytes).subspan(cursor),
                                           live->firmwareSerialState().now, events);
            cursor += step.consumedBytes;
            for (std::size_t i = 0; i < events.count; ++i) {
                const auto &event = events.entries[i];
                converters += event.kind == S::EventKind::Converter;
                pitWrites += event.kind == S::EventKind::ExternalWrite && event.address >= 0x1000 &&
                             event.address <= 0x2300;
                if (event.kind == S::EventKind::SerialRead) {
                    check(reads < bytes.size() && event.value == bytes[reads].value,
                          "direct B2 trace reads exact module-pin decoded byte order");
                    ++reads;
                }
            }
            check(step.status == S::Status::ReachedTarget || step.status == S::Status::OutputFull,
                  "direct B2 trace reaches the same causal audio endpoint");
            if (step.status == S::Status::ReachedTarget)
                break;
        }
        check(cursor == bytes.size() && reads == bytes.size(),
              "all four sender-derived bytes are consumed and read");
        check(oracle.control.ram == live->firmwareSerialState().control.ram,
              "actual engine RAM agrees with direct execution of the qualified B2 trace");
        check(pitWrites > 0 && converters > 0,
              "decoded bytes reach real PIT and converter bus writes");
        check(YouKnowTestAccess::voice(*live).dco.divider > 0,
              "actual voice oscillator receives a physical divider value");
        std::cout << (factor == 1 ? "" : ",") << "{\"factor\":" << factor
                  << ",\"rms\":" << std::sqrt(energy / (2 * count))
                  << ",\"parameter_delta_rms\":" << std::sqrt(difference / (2 * count))
                  << ",\"exact_pre_parameter_samples\":" << exactPrefix
                  << ",\"pit_writes\":" << pitWrites << ",\"converter_events\":" << converters
                  << '}';
    }
    std::cout << "],\"assertions\":" << assertions << ",\"status\":\"PASS\"}\n";
}
