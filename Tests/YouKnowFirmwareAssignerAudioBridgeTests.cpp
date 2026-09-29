// Composition and boundary regression. The normal score anchors below were
// independently pinned by the earlier original-ROM scheduler qualification.
// The bridge is additionally compared across partitions and bounded buffers;
// these are continuation contracts, not new evidence for a UART latch subcycle.
#include "../Source/DSP/YouKnowFirmwareAssignerAudioBridge.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <tuple>
#include <vector>

using B = youknow::FirmwareAssignerAudioBridge;
using A = B::Sender;
namespace U = youknow::FirmwareUartTrace;
namespace D = youknow::FirmwareSerialPinDecoder;
namespace Io = youknow::FirmwareAssignerIo;
static unsigned checks = 0, allocations = 0;
static bool allocationGuard = false;
void* operator new(std::size_t size) {
    if (allocationGuard) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void check(bool value, const char* text) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", text); std::abort(); }
}
struct Fixture {
    B::State state;
    A::Configuration configuration;
    A::Tables tables;
    Io::Inputs inputs;
    Fixture() {
        inputs.panelAdc.fill(132);
        A::State cpu;
        cpu.io.conversion = {0, 0, 0, 255};
        auto& ram = cpu.ram;
        std::fill(ram.begin() + 0x58, ram.begin() + 0x68, 132);
        std::fill(ram.begin() + 0x6c, ram.begin() + 0x7c, 132);
        ram[0x6b] = 255;
        for (unsigned card = 0; card < 6; ++card) {
            ram[0x80 + card] = static_cast<std::uint8_t>(0x88 + card);
            ram[0x88 + card] = 0x80;
        }
        ram[0x86] = 0x88;
        std::fill(ram.begin() + 0x90, ram.begin() + 0xa0, 64);
        ram[0x4f] = 0x21; ram[0xb6] = 2;
        ram[0x8e] = ram[0xb8] = ram[0xc7] = 0x2a;
        ram[0xba] = 16; ram[0xbb] = 1; ram[0xbc] = 8; ram[0xbe] = 12;
        ram[0xc5] = ram[0xc6] = 4; ram[0xc8] = 0x42; ram[0xcb] = 0xfc;
        check(B::reset(state, cpu, {}), "explicit warm reset");
    }
    B::Result run(std::uint64_t target, B::Output& output,
                  std::span<const A::InputEvent> incoming = {}) {
        allocationGuard = true;
        const auto result = B::advanceTo(state, configuration, tables, inputs,
                                         incoming, target, output);
        allocationGuard = false;
        return result;
    }
};
auto registers(const A::Registers& r) {
    return std::tuple{r.a,r.b,r.c,r.d,r.e,r.h,r.l,r.ea,r.v,r.alternateA,r.alternateB,
        r.alternateC,r.alternateD,r.alternateE,r.alternateH,r.alternateL,r.alternateEa,
        r.alternateV,r.pc,r.sp,r.carry,r.skip,r.halfCarry,r.zero,r.l0,r.l1};
}
auto pending(const A::Pending& p) {
    return std::tuple{p.kind,p.start,p.remaining,p.address,p.returnPc,p.savedPsw,p.skipped};
}
auto peripheral(const Io::Peripheral& p) {
    return std::tuple{p.muxLatch,p.portF,p.portB,p.anm,p.conversion,p.channel,
        p.statesUntilConversion,p.request};
}
auto uart(const U::State& u) {
    return std::tuple{u.now,u.transmitEnabled,u.txBufferFull,u.txBuffer,u.fst,
        u.frameActive,u.frameByte,u.bitIndex,u.frameOrdinal,u.frameStart,u.nextBit,
        u.idleLaunch,u.stopCenterObserved,u.portC,u.txd,u.moduleRxD,u.midiLogic};
}
auto receiver(const D::State& r) {
    return std::tuple{r.now,r.moduleLevel,r.instantClosed,r.active,r.frameStart,
        r.nextSample,r.sampleIndex,r.byte,r.pendingStart};
}
bool sameCore(const B::State& x, const B::State& y) {
    const auto& a=x.assigner; const auto& b=y.assigner;
    return a.ram==b.ram && a.patchRam==b.patchRam
        && a.patchRamAvailable==b.patchRamAvailable
        && registers(a.registers)==registers(b.registers)
        && pending(a.pending)==pending(b.pending) && peripheral(a.io)==peripheral(b.io)
        && std::tuple{a.now,a.foregroundPasses,a.mkh,a.eiDeferred,a.interruptEnabled,
                      a.fsr,a.receiveError,a.rxBufferFull,a.rxBuffer}
            ==std::tuple{b.now,b.foregroundPasses,b.mkh,b.eiDeferred,b.interruptEnabled,
                         b.fsr,b.receiveError,b.rxBufferFull,b.rxBuffer}
        && uart(x.uart)==uart(y.uart) && receiver(x.receiver)==receiver(y.receiver)
        && x.completedSourceValid==y.completedSourceValid
        && x.completedSourceThrough==y.completedSourceThrough;
}
using Bytes = std::vector<std::pair<std::uint64_t, unsigned>>;
struct Run { Fixture fixture; Bytes bytes; unsigned full=0, quota=0; };
constexpr std::array<A::InputEvent,5> note{{{100,0x90},{1380,60},{2660,127},
    {10420,60},{11700,0}}};
Run score(std::uint64_t chunk, unsigned capacity) {
    Run r;
    std::size_t consumed=0;
    for (std::uint64_t target=0; target<40000;) {
        target=std::min<std::uint64_t>(40000,target+chunk);
        for (unsigned attempts=0;;++attempts) {
            check(attempts<10000,"bounded score continuation");
            std::array<B::ByteReady,32> storage;
            B::Output output{std::span(storage).first(capacity),0};
            auto result=r.fixture.run(target,output,std::span(note).subspan(consumed));
            consumed+=result.consumedInputs;
            for (std::size_t i=0;i<output.count;++i)
                r.bytes.emplace_back(storage[i].states,storage[i].value);
            if (result.status==B::Status::ReachedTarget) break;
            check(result.status==B::Status::OutputFull || result.status==B::Status::WorkLimit,
                  "score has only resumable pauses");
            r.full+=result.status==B::Status::OutputFull;
            r.quota+=result.status==B::Status::WorkLimit;
        }
    }
    check(consumed==note.size(),"all caller input consumed exactly once");
    check(r.fixture.state.receiver.now==40000 && r.fixture.state.receiver.instantClosed,
          "receiver closes requested endpoint");
    return r;
}
void partitionAndReset() {
    auto whole=score(40000,32);
    // Previous raw-ROM proof fixes TXB at5555,6230,15920. With declared idle
    // phase0, launch5632, contiguous6912, and16000 -> frame ends below.
    const Bytes expected{{6912,0x88},{8192,60},{17280,0x80}};
    if (whole.bytes!=expected) {
        for (auto [t,v]:whole.bytes) std::fprintf(stderr,"byte %llu %u\n",
            static_cast<unsigned long long>(t),v);
    }
    check(whole.bytes==expected,"pinned independent scheduler/baud score anchors");
    for (const auto chunk:{1u,7u,83u,4096u,40000u}) {
        auto split=score(chunk,1);
        check(split.bytes==whole.bytes,"same wire bytes under arbitrary slicing/backpressure");
        check(sameCore(split.fixture.state,whole.fixture.state),
              "every source/receiver state matches after target slicing");
        if (chunk==40000) check(split.full>0,"one-byte sink really backpressures");
    }
    Fixture fresh;
    auto& s=whole.fixture.state;
    check(B::reset(s,fresh.state.assigner,fresh.state.uart),"reset after partial/full stream");
    check(sameCore(s,fresh.state) && !s.batchPending && !s.heldInputsValid,
          "reset clears bridge histories with explicit warm snapshot");
    U::State invalid;
    invalid.bitIndex=10;
    const auto before=s;
    check(!B::reset(s,fresh.state.assigner,invalid) && sameCore(s,before),
          "invalid UART reset is atomic");
    invalid={}; invalid.frameOrdinal=U::noEvent;
    check(!B::reset(s,fresh.state.assigner,invalid),"invalid ordinal reset rejected");
    Fixture inFlight;
    std::array<B::ByteReady,1> unused;
    B::Output output{unused,0};
    const auto active=inFlight.run(6000,output,note);
    check(active.status==B::Status::ReachedTarget && inFlight.state.receiver.active
        && inFlight.state.uart.frameActive,"reset fixture contains a live partial frame");
    check(B::reset(inFlight.state,fresh.state.assigner,fresh.state.uart)
        && sameCore(inFlight.state,fresh.state),"reset abandons both physical partial frames");
}
void chronologyAndPressure() {
    Fixture f;
    B::Output none{{},0};
    const std::array<A::InputEvent,2> atZero{{{0,0,A::InputKind::ReceiveError},
                                           {0,0,A::InputKind::ReceiveError}}};
    auto result=f.run(0,none,atZero);
    check(result.status==B::Status::ReachedTarget && result.consumedInputs==2,
          "initial equal time0 inputs allowed and ordered");
    const auto before=f.state;
    result=f.run(0,none,atZero);
    check(result.status==B::Status::InvalidArgument && sameCore(f.state,before),
          "late input at closed inclusive zero rejected atomically");
    check(f.run(0,none).status==B::Status::ReachedTarget && sameCore(f.state,before),
          "empty repeated completed target is idempotent");
    Fixture g;
    result=g.run(40000,none,note);
    check(result.status==B::Status::OutputFull && g.state.batchPending,
          "zero-capacity sink retains completed frame and wire suffix");
    const auto blocked=g.state;
    auto changed=g.inputs; g.inputs.panelAdc[6]=0;
    check(g.run(40000,none,std::span(note).subspan(result.consumedInputs)).status
            ==B::Status::InvalidArgument && sameCore(g.state,blocked),
          "physical IO cannot change while old source batch is pending");
    g.inputs=changed;
    std::array<B::ByteReady,8> bytes;
    B::Output output{bytes,0};
    auto resumed=g.run(40000,output,std::span(note).subspan(result.consumedInputs));
    check(resumed.status==B::Status::ReachedTarget && output.count==3,
          "all bytes retained through zero-capacity retry");
    g.inputs.panelAdc[6]=0;
    check(g.run(40001,output).status==B::Status::ReachedTarget,
          "held physical IO can change after completed public boundary");
    Fixture longRun;
    result=longRun.run(960000,none);
    check(result.status==B::Status::WorkLimit && longRun.state.assigner.now>0
        && longRun.state.assigner.now<960000,"large target obeys bounded work quota");
    const auto closed=longRun.state.completedSourceThrough;
    const A::InputEvent late{closed,0,A::InputKind::ReceiveError};
    check(longRun.run(960000,none,{&late,1}).status==B::Status::InvalidArgument,
          "WorkLimit does not reopen a completed source boundary");
    unsigned continuations=0;
    while (result.status!=B::Status::ReachedTarget) {
        check(result.status==B::Status::WorkLimit && ++continuations<10,
              "bounded long-run continuation");
        result=longRun.run(960000,none);
    }
    check(longRun.state.receiver.now==960000,"quota continuation reaches exact time");
}
void collision() {
    Fixture f;
    auto& s=f.state;
    // A coherent partial instant: a UART start edge on MIDI has committed;
    // the CPU's actual MOV PC,A at07D7 is due at the same time. Switching PC2
    // then presents that start edge to the module. Previous module frame-end
    // delivery is also due now. This is an explicit boundary fixture, not a
    // claim that the normal guarded route routine produces this starting state.
    s.assigner.now=s.uart.now=1280;
    s.assigner.interruptEnabled=false;
    s.assigner.registers.pc=0x07d7; s.assigner.registers.a=0xfd;
    s.assigner.pending={A::PendingKind::Instruction,1270,0,0x07d7,0,0,false};
    s.uart.frameActive=true; s.uart.frameOrdinal=2; s.uart.frameStart=1280;
    s.uart.frameByte=0xa6; s.uart.bitIndex=0; s.uart.nextBit=1408;
    s.uart.portC=0xf9; s.uart.txd=false; s.uart.moduleRxD=true; s.uart.midiLogic=false;
    s.receiver.now=1216; s.receiver.active=true; s.receiver.frameStart=0;
    s.receiver.nextSample=1280; s.receiver.sampleIndex=10; s.receiver.byte=0x55;
    s.receiver.moduleLevel=true;
    s.pendingWire[0]={1280,2,U::EventKind::FrameBegin,0xff,0,2};
    s.pendingWire[1]={1280,2,U::EventKind::PinLevels,2,0,2};
    s.wireCount=2; s.batchPending=true; s.batchStatus=A::Status::OutputFull;
    const A::InputEvent sameTime{1280,0,A::InputKind::ReceiveError};
    B::Output none{{},0};
    auto result=f.run(1280,none,{&sameTime,1});
    check(result.status==B::Status::OutputFull && result.consumedInputs==1,
          "partial instant accepts unconsumed same-time source event");
    check(s.uart.portC==0xfd && s.receiver.pendingStart && !s.receiver.instantClosed,
          "same-time CPU route write precedes frame-end delivery even under pressure");
    std::array<B::ByteReady,1> storage;
    B::Output output{storage,0};
    result=f.run(1280,output);
    check(result.status==B::Status::ReachedTarget && output.count==1
        && storage[0].states==1280 && storage[0].value==0x55,
          "prior frame delivered once, without sender metadata substitution");
    check(s.receiver.active && s.receiver.frameStart==1280
        && s.receiver.nextSample==1344 && s.receiver.sampleIndex==0,
          "same-time next frame retained at exact source endpoint");
    check(f.run(1280,output,{&sameTime,1}).status==B::Status::InvalidArgument,
          "inclusive completion rejects new equal-time RX events");
}
void frontiersAndMalformed() {
    Fixture f;
    f.state.assigner.registers.pc=0x0cf5;
    f.state.assigner.registers.h=0x20; f.state.assigner.registers.a=99;
    f.state.assigner.interruptEnabled=false;
    B::Output none{{},0};
    auto result=f.run(7,none);
    check(result.status==B::Status::SourceStopped
        && result.sourceStatus==A::Status::UnavailableMemory,
          "unavailable physical patch memory remains explicit frontier");
    check(f.state.assigner.registers.a==99 && f.state.assigner.registers.l==0
        && f.state.assigner.pending.remaining==0 && !f.state.receiver.instantClosed,
          "failed memory read neither invents data nor closes incomplete instant");
    check(f.run(7,none).status==B::Status::SourceStopped,"retry does not invent completion");
    f.state.assigner.patchRamAvailable=true; f.state.assigner.patchRam[0]=177;
    check(f.run(7,none).status==B::Status::ReachedTarget
        && f.state.assigner.registers.a==177 && f.state.assigner.registers.l==1,
          "supplied physical patch memory resumes exact pending read");
    Fixture g;
    auto& s=g.state;
    s.assigner.now=s.uart.now=1281; s.assigner.registers.pc=0x026e;
    s.assigner.interruptEnabled=false;
    s.receiver.now=1216; s.receiver.active=true; s.receiver.frameStart=0;
    s.receiver.nextSample=1280; s.receiver.sampleIndex=10; s.receiver.byte=0x69;
    s.pendingWire[0]={1280,1,U::EventKind::FrameEnd,0xff,9,7};
    s.wireCount=1; s.batchPending=true; s.batchStatus=A::Status::UnsupportedPath;
    std::array<B::ByteReady,1> bytes;
    B::Output output{bytes,0};
    result=g.run(1400,output);
    check(result.status==B::Status::SourceStopped && output.count==1
        && bytes[0].states==1280 && bytes[0].value==0x69,
          "completed prefix byte drained before nonresumable source diagnostic");
    check(g.run(1400,none).status==B::Status::SourceStopped
        && s.assigner.now==1281,"unsupported PC never turns into false ReachedTarget");
    Fixture invalid;
    const auto before=invalid.state;
    const std::array<A::InputEvent,2> unordered{{{2,0},{1,0}}};
    check(invalid.run(3,none,unordered).status==B::Status::InvalidArgument
        && sameCore(invalid.state,before),"unordered caller input atomically rejected");
    std::array<A::InputEvent,B::maximumInputEvents+1> excess;
    check(invalid.run(3,none,excess).status==B::Status::InvalidArgument,
          "input validation itself has a fixed work bound");
    invalid.configuration.uart.idleBaudGridPhase=128;
    check(invalid.run(3,none).status==B::Status::InvalidArgument,
          "invalid UART scenario rejected before source mutation");
}
int main() {
    partitionAndReset(); chronologyAndPressure(); collision(); frontiersAndMalformed();
    check(allocations==0,"all observed bridge calls allocate zero heap memory");
    std::printf("PASS %u bridge assertions\n",checks);
}
