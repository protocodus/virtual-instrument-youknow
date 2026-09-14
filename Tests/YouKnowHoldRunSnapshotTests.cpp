#include "../Source/DSP/YouKnowEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace youknow {
struct YouKnowTestAccess {
    static bool running(const YouKnowEngine& e) { return e.voices_[0].envelope.running; }
    static bool keyed(const YouKnowEngine& e) { return e.voices_[0].keyDown; }
    static bool sustained(const YouKnowEngine& e) { return e.voices_[0].sustained; }
    static bool reset(const YouKnowEngine& e) { return e.voices_[0].dcoResetPending; }
    static unsigned delay(const YouKnowEngine& e) { return e.lfoDelayByte_; }
};
}

namespace {
using Engine = youknow::YouKnowEngine;
using Access = youknow::YouKnowTestAccess;
int failures = 0;
void require(bool okay, const char* message) {
    if (!okay && failures++ < 20) std::cerr << message << '\n';
}
void process(Engine& e, int frames, int block = 73, std::vector<float>* audio = nullptr) {
    std::vector<float> left(block), right(block);
    while (frames > 0) {
        const int count = std::min(frames, block);
        e.process(left.data(), right.data(), count);
        if (audio) audio->insert(audio->end(), left.begin(), left.begin() + count);
        frames -= count;
    }
}
std::unique_ptr<Engine> held(double rate, int factor, Engine::ConverterTimingProfile timing) {
    auto e = std::make_unique<Engine>();
    e->selectConverterTimingProfile(timing);
    youknow::EngineParameters p;
    p.keyMode = youknow::KeyMode::Poly2; // first released slot is reused
    p.attack = 0; p.decay = .5f; p.sustain = .8f; p.release = .6f;
    p.lfoDelay = 0; p.dcoLfoDepth = 1; p.lfoRate = .65f;
    p.calibration = p.velocityDepth = p.noiseLevel = 0;
    e->setParameters(p);
    e->prepare(rate, factor);
    e->noteOn(60, 1);
    process(*e, static_cast<int>(rate * .05));
    // Changing the delay coefficient after it has faded does not restart
    // onset. This reaches the branch fixture through the public controls
    // without spending several seconds per sample-rate/quality combination.
    p.lfoDelay = .6f;
    e->setParameters(p);
    e->setSustainPedal(true);
    e->noteOff(60);
    process(*e, static_cast<int>(rate * .02));
    require(Access::running(*e) && !Access::keyed(*e) && Access::sustained(*e),
            "fixture must retain FF11 after the key gate clears under HOLD");
    require(Access::delay(*e) == 255, "fixture must reach full LFO onset depth");
    require(!Access::reset(*e), "fixture must consume the preceding reset request");
    return e;
}

// Independent branch witnesses from the published B-2 listing:
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L155-L160
// HOLD-off at 00BF only clears FF1E bit 0 and RETIs. The old FF11 remains
// until 02FD..02FF. A changed Voice On tests that byte at 012E: FF11 set
// bypasses 013E's reset request. At 030D the onset path likewise reads FF11;
// without a preceding zero-mask observation at 036B, 0312 cannot restart it.
void checkBranches() {
    for (const auto timing : {Engine::ConverterTimingProfile::MeasuredChartGeometry,
                               Engine::ConverterTimingProfile::FirmwareControlNoInterrupt})
        for (double rate : {44100., 48000., 96000.})
            for (int factor : {1, 4}) {
                auto before = held(rate, factor, timing);
                before->setSustainPedal(false);
                require(Access::running(*before) && !Access::sustained(*before),
                        "HOLD-off must preserve the preceding FF11 snapshot");
                before->noteOn(72, 1);
                require(!Access::reset(*before),
                        "changed note before FF11 clears must take count-only path");
                require(Access::delay(*before) == 255,
                        "unobserved idle interval must not restart the LFO onset");
                process(*before, static_cast<int>(rate * .02));
                require(Access::delay(*before) == 255,
                        "the resumed pass must preserve fully faded LFO depth");

                auto after = held(rate, factor, timing);
                after->setSustainPedal(false);
                process(*after, static_cast<int>(rate * .01));
                require(!Access::running(*after), "idle pass must clear FF11 after HOLD-off");
                after->noteOn(72, 1);
                require(Access::reset(*after),
                        "changed note after FF11 clears must request Mode-3 reset");
                process(*after, static_cast<int>(rate * .01));
                require(Access::delay(*after) == 0,
                        "an observed idle interval must restart the LFO onset");

                auto equal = held(rate, factor, timing);
                equal->setSustainPedal(false);
                process(*equal, static_cast<int>(rate * .01));
                equal->noteOn(60, 1);
                require(!Access::reset(*equal), "equal note must bypass reset even with FF11 clear");
            }
}

std::vector<float> render(int block) {
    auto e = held(48000, 1, Engine::ConverterTimingProfile::MeasuredChartGeometry);
    std::vector<float> result;
    e->setSustainPedal(false);
    e->noteOn(72, 1);
    process(*e, 24000, block, &result);
    e->noteOff(72);
    process(*e, 12000, block, &result);
    return result;
}
void checkRender() {
    const auto one = render(1), many = render(113);
    require(one == many, "HOLD release/reassignment audio must be host-block invariant");
    require(std::all_of(one.begin(), one.end(), [](float x) { return std::isfinite(x); }),
            "HOLD release/reassignment audio must remain finite");
}
}
int main() {
    checkBranches(); checkRender();
    std::cout << (failures ? "FAIL " : "PASS ") << "HoldRunSnapshot failures=" << failures << '\n';
    return failures ? 1 : 0;
}
