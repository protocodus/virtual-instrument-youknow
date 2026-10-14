#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace youknow
{
struct YouKnowTestAccess
{
    static float cutoff(float low, float high, float position, float character,
                        float drift = 0.0f)
    {
        YouKnowEngine::VoiceCard card;
        card.cutoffOffsetError = low;
        card.cutoffScaleError = high;
        card.driftValue = drift;
        const float counts = YouKnowEngine::vcfFreqTrimAnchorCounts
            + position * YouKnowEngine::vcfWidthTrimSpanCounts;
        return (YouKnowEngine::cutoffAnalogCounts(counts, card, character, 0) - counts)
            * 1200.0f / YouKnowEngine::vcfCountsPerOctave;
    }
    static float resonance(float cv, float draw, float character)
    {
        YouKnowEngine::VoiceCard card;
        card.resonanceError = draw;
        return YouKnowEngine::resonanceFeedbackFor(cv, card, character, true, true);
    }
    static std::array<float, 3> controls(YouKnowEngine& e, unsigned code,
                                       float draw, float character)
    {
        auto& v = e.voices_[0];
        v.cardIndex = 0;
        v.envelope.value = static_cast<float>(code) / 4095.0f;
        e.cards_[0].vcaControlOffset = draw;
        e.activeParameters_.calibration = character;
        e.activeParameters_.vcaMode = VcaMode::Envelope;
        e.activeParameters_.velocityDepth = 0;
        const YouKnowEngine::ConverterWrite write {
            YouKnowEngine::ConverterDestination::VoiceVca, 0 };
        const auto& order = YouKnowEngine::converterWriteOrder();
        for (std::size_t n = 0; n < order.size(); ++n)
            if (order[n].destination == write.destination && order[n].voice == 0)
                e.firmwareConverterCodes_[n] = static_cast<std::uint16_t>(code);
        return { e.voiceVcaTarget(v, e.activeParameters_),
                 e.firmwareConverterTarget(write), e.firmwareSerialDacTarget(write, code) };
    }
    static std::array<double, 2> gainAndCoupling(YouKnowEngine& e, float draw)
    {
        e.cards_[0].vcaGainError = draw;
        e.refreshVoiceVcaCoupling();
        e.updateVoiceAudio(e.voices_[0], e.activeParameters_);
        return { e.voices_[0].vcaInputTrim, e.cards_[0].vcaInputCouplingG };
    }
    static double thermalScale(const EngineParameters& p, int card, float warmup)
    { return YouKnowEngine::thermalFilterOmegaScaleFor(p, card, warmup); }
    static double gradient(int card) { return YouKnowEngine::chassisGradientCelsius(card); }
    static double meanGradient() { return YouKnowEngine::chassisGradientMeanCelsius(); }
    static double gradientPeak() { return YouKnowEngine::chassisGradientPeakCelsius; }
    static double acceptance() { return YouKnowEngine::vcfTrimAcceptanceCents; }
    static double wanderRms()
    {
        auto engine = std::make_unique<YouKnowEngine>();
        YouKnowEngine::VoiceCard card;
        card.driftState = 17;
        double sum = 0, squares = 0;
        constexpr int count = 1000000;
        for (int n = -10000; n < count; ++n)
        {
            engine->updateVoiceCardDrift(card);
            if (n < 0) continue;
            const double cents = (YouKnowEngine::cutoffAnalogCounts(0, card, 1, 0))
                * 1200.0 / YouKnowEngine::vcfCountsPerOctave;
            sum += cents;
            squares += cents * cents;
        }
        return std::sqrt(squares / count - (sum / count) * (sum / count));
    }
};
}

namespace
{
using namespace youknow;
using Access = YouKnowTestAccess;
unsigned assertions = 0;
void require(bool condition, const char* text)
{
    ++assertions;
    if (!condition) { std::fprintf(stderr, "Voice residual contract: %s\n", text); std::exit(1); }
}
void near(double observed, double expected, double tolerance, const char* text)
{
    if (!(std::isfinite(observed) && std::abs(observed - expected) <= tolerance))
        std::fprintf(stderr, "observed %.12g, expected %.12g\n", observed, expected);
    require(std::isfinite(observed) && std::abs(observed - expected) <= tolerance, text);
}
void checkServiceBoundsAndWander()
{
    near(Access::acceptance(), 10, 0, "manufacturer service window changed");
    // A serviced pair of checkpoints has independent residuals inside a
    // deliberately narrower +/-2.5-cent prior. Linear interpolation cannot
    // exceed those endpoints; do not assert that extrapolation is bounded.
    for (float low : { -1.f, 0.f, 1.f }) for (float high : { -1.f, 0.f, 1.f })
    for (float character : { 0.f, 1.f, 2.f })
    for (float position : { 0.f, .25f, .5f, .75f, 1.f })
    {
        const double expected = ((1 - position) * low + position * high) * 2.5 * character;
        near(Access::cutoff(low, high, position, character), expected, .0006,
             "service residual or Character scaling differs from the declared estimate");
        require(std::abs(Access::cutoff(low, high, position, character)) <= 5.001,
                "checked-span residual exceeds even Character 2 bounds");
    }
    near(Access::cutoff(0, 0, 0, 1, 1), 8.0 * 1200 / 1143, .0006,
         "wander multiplier is not eight converter counts");
    const double analytic = .004 / std::sqrt(3 * (1 - .9992 * .9992)) * 8 * 1200 / 1143;
    near(analytic, .4850106, .000001, "independent AR variance derivation changed");
    const double measured = Access::wanderRms();
    near(measured, analytic, .05, "deterministic AR run exceeds conservative RMS prior");
    std::printf("cutoff wander: theoretical %.9f, deterministic %.9f cents RMS\n", analytic, measured);
}
void checkResonanceAndControlPaths()
{
    auto engine = std::make_unique<YouKnowEngine>();
    for (float character : { 0.f, 1.f, 2.f }) for (float draw : { -1.f, 0.f, 1.f })
    {
        // A CV residual is bounded between neighboring unperturbed controls,
        // including clamped zero/full endpoints. It is not a gain percentage.
        for (float cv : { 0.f, .5f, 1.f })
        {
            const float expectedCv = std::clamp(cv + draw * .002f * character, 0.f, 1.f);
            near(Access::resonance(cv, draw, character), Access::resonance(expectedCv, 0, 0), 1e-7,
                 "RES residual changed coordinate, units, or endpoint clipping");
        }
        for (unsigned code : { 0u, 2048u, 4095u })
        {
            const auto targets = Access::controls(*engine, code, draw, character);
            const float expected = std::clamp(static_cast<float>(code) / 4095.f
                + draw * .001f * character, 0.f, 1.f);
            for (float value : targets)
                near(value, expected, 1e-7, "direct, firmware, and serial VCA controls disagree");
        }
    }
    near(Access::resonance(0, -1, 1), Access::resonance(0, 0, 0), 0,
         "negative RES residual escaped the zero clamp");
    near(Access::resonance(1, 1, 1), Access::resonance(1, 0, 0), 0,
         "positive RES residual escaped the full clamp");
}
void checkGainAndThermalResidual()
{
    auto e = std::make_unique<YouKnowEngine>();
    require(e->configureServiceDerivedVcaCoupling(true), "service coupling configuration rejected");
    EngineParameters p;
    p.enableSpatialThermalGradient = false;
    e->setParameters(p);
    e->prepare(48000, 64, 1);
    const auto zero = Access::gainAndCoupling(*e, 0);
    for (float draw : { -1.f, 1.f })
    {
        const auto value = Access::gainAndCoupling(*e, draw);
        near(value[0], 1 + draw * .01, 1e-7, "VCA input gain exceeds its one-percent prior");
        // C59's atan(g)=dt/(2RC). At unchanged temperature, VR27's divider
        // sets 1/R proportional to the input trim. This catches a stale
        // three-percent coefficient in the separate coupling calculation.
        near(std::atan(value[1]) / std::atan(zero[1]), value[0], 2e-7,
             "C59 service coupling does not follow the same input trim");
    }
    p.enableSpatialThermalGradient = true;
    for (int card = 0; card < 6; ++card)
    {
        near(Access::thermalScale(p, card, 0), 1, 0,
             "gradient appeared before warmup");
        const double expected = 1 + .00033 * (Access::gradient(card)
            - Access::meanGradient());
        near(Access::thermalScale(p, card, 1), expected, 1e-10,
             "compensated cutoff residual is not the declared 0.033 percent per C");
    }
    near(YouKnowEngine::thermalWarmupTimeConstantSeconds, 3, 0, "user-selected startup time changed");
    near(Access::gradientPeak(), 4, 0, "heat gradient was changed with cutoff compensation");
}
void checkSilenceAndRetirement()
{
    for (bool product : { false, true }) for (float character : { 0.f, 1.f, 2.f })
    {
        auto e = std::make_unique<YouKnowEngine>();
        EngineParameters p;
        if (product)
        {
            require(ActiveProductFidelityProfile::tryConfigureBeforePrepare(*e), "product configuration rejected");
            ActiveProductFidelityProfile::applyTo(p);
        }
        p.calibration = character;
        p.keyMode = KeyMode::Unison;
        p.attack = 0; p.decay = 0; p.sustain = .5f; p.release = 0;
        p.chorus = ChorusMode::Off; p.chorusNoise = 0;
        p.volume = .8f; p.vcaLevel = .8f;
        p.vcfSolverMode = VcfSolverMode::Rk4Single;
        e->setParameters(p); e->prepare(48000, 64, 1);
        e->noteOn(60, 1);
        std::array<float, 64> l {}, r {};
        for (int block = 0; block < 1600; ++block)
        {
            if (block == 32) e->noteOff(60);
            e->process(l.data(), r.data(), 64);
            for (std::size_t n = 0; n < l.size(); ++n)
                require(std::isfinite(l[n]) && std::isfinite(r[n]), "residuals produced nonfinite audio");
        }
        require(e->getActiveVoiceCount() == 0, "positive VCA residual prevented released cards retiring");
        // Keep physical circuit noise enabled. The existing dry-floor
        // contract is below -100 dBFS RMS at Character 1; allow the declared
        // linear Character scaling at 2 rather than demanding digital zero.
        double energy = 0;
        constexpr int idleBlocks = 768;
        for (int block = 0; block < idleBlocks; ++block)
        {
            e->process(l.data(), r.data(), 64);
            for (std::size_t n = 0; n < l.size(); ++n)
                energy += static_cast<double>(l[n]) * l[n] + static_cast<double>(r[n]) * r[n];
        }
        const double rms = std::sqrt(energy / (2 * 64 * idleBlocks));
        require(std::isfinite(rms) && rms < 1e-5 * std::max(1.f, character),
                "retired voices left audio above the established dry circuit floor");
        std::printf("post-retirement %s Character %.0f: %.9g RMS\n",
                    product ? "product" : "reference", character, rms);
        e->reset();
        require(e->getActiveVoiceCount() == 0, "reset left active cards");
    }
}
}

int main()
{
    checkServiceBoundsAndWander();
    checkResonanceAndControlPaths();
    checkGainAndThermalResidual();
    checkSilenceAndRetirement();
    std::printf("PASS %u conservative voice residual assertions\n", assertions);
}
