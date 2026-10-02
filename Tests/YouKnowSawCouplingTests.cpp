// Roland's original p.9 MC5534A drawing sends the unipolar Miller ramp
// through a passive resistor leg to WAVE. SAW OFF grounds the leg through
// its diode/pin17/Tr24; C56/C50 then remove the settled mean (p.13).
// https://www.synfo.nl/servicemanuals/Roland/ROLAND_JUNO-106_SERVICE_NOTES_1st.pdf
// These fixtures qualify that topology in the engine's existing voiced mixer
// coordinate, not an installed-unit voltage or diode-clamp measurement.
#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static void setup(YouKnowEngine& engine, double rate, bool couple, bool fast)
    {
        engine.prepare(rate, 64, false);
        EngineParameters p;
        p.calibration = 0.0f;
        p.aging = 0.0f;
        p.sawEnabled = true;
        p.pulseEnabled = false;
        p.subLevel = 0.0f;
        p.noiseLevel = 0.0f;
        p.cutoff = 1.0f;
        p.resonance = 0.0f;
        p.chorus = ChorusMode::Off;
        p.enableSawUnipolarNodeCoupling = couple;
        p.enablePulseOffWaveNodeCoupling = false;
        p.enableSubHalfWaveNodeCoupling = false;
        p.vcfTanhMode = fast ? VcfTanhMode::PolyZoned : VcfTanhMode::Exact;
        engine.setParameters(p);

        // Hold the ramp at its physical 6 V midpoint to isolate the DC
        // origin from clock/reset/BLEP and nonlinear-filter approximations.
        // The two configurations retain identical oscillator/control state.
        auto& voice = engine.voices_[0];
        voice.dco.reset();
        voice.dco.rampValue = 0.0;
        voice.dco.renderScale = 1.0f;
        voice.dco.rampSlopePerSecond = 0.0;
        voice.rampCurrentScale = 1.0f;
        voice.active = !fast;
        voice.dco.saw.prime(0.0f);
    }

    static double state(const YouKnowEngine& engine)
    {
        return engine.voices_[0].moduleCoupling.state;
    }

    static double origin(const YouKnowEngine& engine)
    {
        return engine.sawWaveNodeOffset(engine.activeParameters_);
    }

    static void saw(YouKnowEngine& engine, bool on)
    {
        // The command's digital routing is tested separately. Keep the
        // capacitor's charge intact at this precise analog switch boundary.
        engine.activeParameters_.sawEnabled = on;
    }

    static double step(YouKnowEngine& engine)
    {
        const auto frame = engine.prepareVoiceFilter(
            engine.voices_[0], engine.activeParameters_, 0.0f);
        return frame.input;
    }

    static void restoreRunningOscillator(YouKnowEngine& engine)
    {
        auto& voice = engine.voices_[0];
        auto& dco = voice.dco;
        dco.divider = dco.pendingDivider = 7675;
        dco.periodSamples = engine.oversampledRate_ * 7675.0 / 2000000.0;
        dco.pitState = YouKnowEngine::Dco::PitState::running;
        dco.pitOutHigh = true;
        dco.pitClocksToEvent = 7675.0 / 2.0;
        voice.dcoCv = voice.dcoCvTarget = 256.0f;
        voice.dcoPitchTransactionValid = false;
        engine.beginDcoCharge(voice, 0.0f, false);
        dco.saw.prime(static_cast<float>(
            (dco.rampValue + 1.0) * dco.renderScale - 1.0));
    }

    static void clock(YouKnowEngine& engine)
    {
        engine.advanceRangeClock(engine.activeParameters_.range);
    }
};
}

namespace
{
using Probe = youknow::YouKnowTestAccess;
using youknow::YouKnowEngine;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void checkPoweredStartupAndContinuousSwitchResponse(double rate, bool fast)
{
    auto candidate = std::make_unique<YouKnowEngine>();
    auto reference = std::make_unique<YouKnowEngine>();
    Probe::setup(*candidate, rate, true, fast);
    Probe::setup(*reference, rate, false, fast);
    const double origin = Probe::origin(*candidate);
    require(origin > 0.0 && Probe::origin(*reference) == 0.0,
            "the explicit saw-origin comparison is unavailable");
    require(std::abs(Probe::state(*candidate) - Probe::state(*reference) - origin) < 1e-6,
            "a powered startup did not prime C56 from the unipolar saw mean");

    // Constant SAW ON must not replay the mean as an artificial note-on or
    // resume transient. This also exercises saw-only tracking behind a closed
    // VCA when pulse/sub node coupling comparisons are both disabled.
    const int heldSamples = static_cast<int>(rate * 0.02);
    double worstHeldDifference = 0.0;
    for (int sample = 0; sample < heldSamples; ++sample)
    {
        const double a = Probe::step(*candidate);
        const double b = Probe::step(*reference);
        worstHeldDifference = std::max(worstHeldDifference, std::abs(a - b));
    }
    require(worstHeldDifference < 2e-6,
            "a held saw replayed its DC origin into the filter");

    // Independent continuous capacitor equation: after a -origin step,
    // d(charge difference)/dt = -difference/(R*C). The existing C56 network
    // has a voiced resistance and an anchored 10 uF capacitance; read its
    // exposed resistance, then integrate the continuous law independently.
    const double tau = reference->moduleInputCouplingResistanceOhms() * 10e-6;
    Probe::saw(*candidate, false);
    Probe::saw(*reference, false);
    const int switchedSamples = static_cast<int>(rate * 0.12);
    double worstChargeError = 0.0;
    double worstInputError = 0.0;
    double initialInputDifference = 0.0;
    for (int sample = 0; sample < switchedSamples; ++sample)
    {
        const double a = Probe::step(*candidate);
        const double b = Probe::step(*reference);
        const double time = (sample + 1.0) / rate;
        const double expectedCharge = origin * std::exp(-time / tau);
        worstChargeError = std::max(worstChargeError, std::abs(
            Probe::state(*candidate) - Probe::state(*reference) - expectedCharge));
        if (!fast)
        {
            // Normalize away the separately voiced WAVE-to-filter level;
            // this test qualifies the capacitor's law, not that calibration.
            if (sample == 0)
                initialInputDifference = a - b;
            const double expectedInput = initialInputDifference * std::exp(-sample / (rate * tau));
            worstInputError = std::max(worstInputError, std::abs(a - b - expectedInput));
        }
    }
    require(worstChargeError < 3e-6,
            "SAW OFF does not preserve/discharge C56's stored unipolar mean");
    require(worstInputError < 3e-6,
            "SAW OFF misses the continuous C56 high-pass step response");
    if (!fast)
        require(initialInputDifference < -0.1 * origin,
                "SAW OFF did not send its negative DC step into the filter");

    // Interrupted OFF, then ON: retain the old charge. The next output must
    // pass only the uncharged remainder; a reset to either zero or +origin
    // would erase that capacitor memory.
    const double previousCharge = Probe::state(*candidate) - Probe::state(*reference);
    Probe::saw(*candidate, true);
    Probe::saw(*reference, true);
    const double a = Probe::step(*candidate);
    const double b = Probe::step(*reference);
    const double expectedCharge = origin + (previousCharge - origin) * std::exp(-1.0 / (rate * tau));
    require(std::abs(Probe::state(*candidate) - Probe::state(*reference) - expectedCharge) < 3e-6,
            "an interrupted SAW switch discarded C56 charge");
    if (!fast)
        require(std::abs(a - b + initialInputDifference * (origin - previousCharge) / origin) < 3e-6,
                "SAW ON replayed the full mean after a short OFF interval");
}

void checkRunningSawAcIsUnchanged()
{
    constexpr double rate = 192000.0;
    auto candidate = std::make_unique<YouKnowEngine>();
    auto reference = std::make_unique<YouKnowEngine>();
    Probe::setup(*candidate, rate, true, false);
    Probe::setup(*reference, rate, false, false);
    Probe::restoreRunningOscillator(*candidate);
    Probe::restoreRunningOscillator(*reference);
    double worstDifference = 0.0;
    double peak = 0.0;
    for (int sample = 0; sample < 8192; ++sample)
    {
        const double a = Probe::step(*candidate);
        const double b = Probe::step(*reference);
        worstDifference = std::max(worstDifference, std::abs(a - b));
        peak = std::max(peak, std::abs(b));
        Probe::clock(*candidate);
        Probe::clock(*reference);
    }
    require(peak > 1.0, "the running-saw equality fixture did not exercise a waveform");
    require(worstDifference < 3e-6,
            "restoring the saw's DC origin changed its steady AC waveform");
}
}

int main()
{
    try
    {
        for (const double rate : { 8000.0, 48000.0, 192000.0 })
            for (const bool fast : { false, true })
                checkPoweredStartupAndContinuousSwitchResponse(rate, fast);
        checkRunningSawAcIsUnchanged();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
