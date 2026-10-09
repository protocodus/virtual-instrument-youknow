// Emit the qualified scheduler scenarios' complete ledgers and final states.
// The renamed test entry point is deliberately unused; call its suites below.
#define main qualifiedSchedulerTestMain
#include "../../Tests/YouKnowFirmwareAssignerSchedulerTests.cpp"
#undef main
#include <iostream>

template<class... Values> void row(Values... values) {
    ((std::cout << static_cast<unsigned long long>(values) << ' '), ...);
    std::cout << '\n';
}
void dump(const schedulerScenarioRegression::Run& run) {
    using namespace schedulerScenarioRegression;
    row(run.events.size(), run.wire.size(), run.outputStops);
    for (const auto& e:run.events) row(e.kind,e.states,e.pc,e.address,e.value);
    for (const auto& e:run.wire) row(e.kind,e.states,e.frameOrdinal,e.value,e.bitIndex,e.pinLevels);
    const auto& s=run.fixture.state;
    for (auto v:s.ram) row(v);
    for (auto v:s.patchRam) row(v);
    std::apply([](auto... v){row(v...);},registers(s.registers));
    std::apply([](auto... v){row(v...);},pending(s.pending));
    row(s.io.muxLatch,s.io.portF,s.io.portB,s.io.anm,s.io.channel,s.io.statesUntilConversion,s.io.request);
    for (auto v:s.io.conversion) row(v);
    row(s.now,s.foregroundPasses,s.mkh,s.eiDeferred,s.interruptEnabled,s.fsr,s.receiveError,s.rxBufferFull,s.rxBuffer);
    std::apply([](auto... v){row(v...);},uart(run.fixture.uart));
}
int main() {
    schedulerScenarioRegression::run();
    schedulerCoreRegression::run(check);
    schedulerBoundaryRegression::run();
    row(assertions);
    using namespace schedulerScenarioRegression;
    constexpr std::array<S::InputEvent,5> note{{{100,0x90},{1380,60},{2660,127},{10420,60},{11700,0}}};
    constexpr std::array<S::InputEvent,7> cutoff{{{100,240},{1380,65},{2660,50},{3940,0},{5220,5},{6500,20},{7780,247}}};
    for (const unsigned phase:{0u,37u,127u})
        for (const unsigned chunk:{1u,7u,83u,4096u,40006u})
            for (const bool pressure:{false,true}) {
                Fixture fixture;
                fixture.configuration.uart.idleBaudGridPhase=phase;
                row(phase,chunk,pressure,0);
                dump(execute(fixture,note,40006,chunk,pressure?8:256,pressure));
                row(phase,chunk,pressure,1);
                dump(execute(fixture,cutoff,40006,chunk,pressure?8:256,pressure));
            }
}
