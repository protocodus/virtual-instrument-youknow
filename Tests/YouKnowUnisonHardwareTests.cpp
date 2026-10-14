// B-2 0280 seeds every FF09 pitch byte to MIDI60; its first zero-coefficient
// 03D7..03F9 pass transfers that byte into each FF71 8.8 glide word. Voice On
// 010D..0147 writes pitch/gate/phase, not FF71, regardless of A-5 key mode.
// https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt
// The actual A-5/UART/B-2 path independently witnesses the Voice On behavior
// below, starting from its declared warm image with those startup words. The
// chart adapter must preserve each word, without claiming that its host-event
// or converter timing equals the original serial pipeline.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace youknow {
struct YouKnowTestAccess {
    static float pitch(const YouKnowEngine& e, int card) { return e.voices_[card].currentMidi; }
    static bool gate(const YouKnowEngine& e, int card) { return e.voices_[card].keyDown; }
    static bool history(const YouKnowEngine& e, int card) { return e.voices_[card].hasVoicePitchHistory; }
    static bool rescan(const YouKnowEngine& e) { return e.assignmentRescanPending_; }
};
}

namespace {
using namespace youknow;
using Access = YouKnowTestAccess;
unsigned assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::fprintf(stderr, "Unison hardware contract: %s\n", message); std::exit(1); }
}
void advance(YouKnowEngine& engine, int count) {
    std::array<float, 64> left {}, right {};
    while (count) {
        const int n = std::min(64, count);
        engine.process(left.data(), right.data(), n);
        for (int i = 0; i < n; ++i)
            require(std::isfinite(left[i]) && std::isfinite(right[i]), "finite audio");
        count -= n;
    }
}
EngineParameters patch(KeyMode mode = KeyMode::Unison, int voices = 6) {
    EngineParameters p;
    p.keyMode = mode; p.polyphony = voices; p.portamento = .8f;
    p.chorus = ChorusMode::Off;
    return p;
}
auto create(const EngineParameters& p, bool physical = true,
            bool original = false, double rate = 48000, int factor = 1) {
    auto e = std::make_unique<YouKnowEngine>();
    require(e->configurePhysicalVoicePowerOnGlide(physical), "configure physical glide policy");
    require(e->configureSingleVoiceLastNotePriority(true), "configure product mono extension");
    e->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    auto parameters = p;
    if (original) {
        require(ActiveProductFidelityProfile::tryConfigureBeforePrepare(*e),
                "configure the native product's current fidelity profile");
        ActiveProductFidelityProfile::applyTo(parameters);
    }
    e->setParameters(parameters);
    e->setOriginalPerformanceMode(original);
    e->prepare(rate, 64, factor);
    return e;
}

void originalStartupOracle() {
    auto e = create(patch(), true, true);
    advance(*e, 4096);
    e->noteOn(48, 1);
    std::array<bool, 6> seen {};
    for (int frame = 0; frame < 2048 && !std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }); ++frame) {
        advance(*e, 1);
        require(e->originalPerformanceHealthy(), "literal A-5/UART/B-2 path remains healthy");
        for (int card = 0; card < 6; ++card)
            if (!seen[card] && Access::gate(*e, card)) {
                seen[card] = true;
                require(Access::pitch(*e, card) == 60.f,
                        "B-2 Voice On preserves the addressed card's startup glide word");
            }
    }
    require(std::all_of(seen.begin(), seen.end(), [](bool b) { return b; }),
            "A-5 assigns all six original Unison cards");
}

void physicalStartup() {
    for (bool physicalPoly : { false, true })
    for (double rate : { 44100., 48000., 96000. }) for (int factor : { 1, 2, 4 }) {
        auto e = create(patch(), physicalPoly, false, rate, factor);
        e->noteOn(48, 1);
        for (int card = 0; card < 6; ++card)
            require(Access::gate(*e, card) && Access::pitch(*e, card) == 60.f,
                    "first physical Unison note retains each powered MIDI60 word");
        advance(*e, 1024);
        for (int card = 0; card < 6; ++card)
            require(Access::pitch(*e, card) < 60.f && Access::pitch(*e, card) > 48.f,
                    "the normal converter pass glides each card toward its new pitch");
        e->reset();
        e->noteOn(72, 1);
        for (int card = 0; card < 6; ++card)
            require(Access::pitch(*e, card) == 60.f, "reset retains physical Unison startup by default");
    }
}

void partialPolyHistory() {
    auto e = create(patch(KeyMode::Poly2));
    e->noteOn(48, 1); advance(*e, 4096);
    e->noteOn(72, 1); advance(*e, 4096);
    require(Access::pitch(*e, 0) < 60.f && Access::pitch(*e, 1) > 60.f,
            "poly fixture has independent glide histories on two cards");
    for (int card = 2; card < 6; ++card)
        require(!Access::history(*e, card) && Access::pitch(*e, card) == 60.f,
                "remaining physical cards retain their unused powered word");
    e->setParameters(patch());
    require(Access::rescan(*e), "mode transition uses the existing assignment rescan");
    int elapsed = 0;
    while (Access::rescan(*e) && elapsed++ < 1024) advance(*e, 1);
    require(!Access::rescan(*e), "Unison rescan finishes");
    require(Access::pitch(*e, 0) < 60.f && Access::pitch(*e, 1) > 60.f,
            "entering Unison preserves different histories instead of aligning physical cards");
    for (int card = 2; card < 6; ++card)
        require(Access::gate(*e, card) && Access::pitch(*e, card) == 60.f,
                "fresh cards entering Unison keep their own startup word, not the new note");
}

void retainedProductExtensions() {
    auto reference = create(patch(), false);
    reference->noteOn(48, 1);
    for (int card = 0; card < 6; ++card)
        require(Access::pitch(*reference, card) == 60.f, "canonical Unison retains physical startup without a host opt-in");
    auto referencePoly = create(patch(KeyMode::Poly2), false);
    referencePoly->noteOn(48, 1);
    require(Access::pitch(*referencePoly, 0) == 48.f,
            "unconfigured direct Poly keeps its established first-note policy");
    auto mono = create(patch(KeyMode::Unison, 1));
    mono->noteOn(48, 1);
    require(Access::pitch(*mono, 0) == 48.f, "single-voice product extension keeps first-note startup");
    mono->noteOn(72, 1); advance(*mono, 1024);
    const float wideningOrigin = Access::pitch(*mono, 0);
    mono->setParameters(patch());
    int elapsed = 0;
    while (Access::rescan(*mono) && elapsed++ < 1024) advance(*mono, 1);
    require(!Access::rescan(*mono), "widening an established stack completes");
    for (int card = 1; card < 6; ++card)
        require(Access::pitch(*mono, card) == wideningOrigin,
                "explicit product widening still adopts its saved pre-rescan stack origin");
    auto extended = create(patch(KeyMode::Unison, 8));
    extended->noteOn(72, 1);
    for (int card = 0; card < 8; ++card)
        require(Access::pitch(*extended, card) == (card < 6 ? 60.f : 72.f),
                "physical and extra cards retain their distinct startup policies");
}
}

int main() {
    originalStartupOracle();
    physicalStartup();
    partialPolyHistory();
    retainedProductExtensions();
    std::printf("PASS %u Unison hardware glide and preserved-extension assertions\n", assertions);
}
