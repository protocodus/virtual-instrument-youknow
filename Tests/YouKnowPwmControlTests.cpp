#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace youknow
{
struct YouKnowTestAccess
{
    static std::array<double, 2> advance(std::array<double, 2> state,
                                       float target, double seconds)
    {
        const auto result = YouKnowEngine::advancePwmHold(
            { state[0], state[1] }, target,
            YouKnowEngine::pwmHoldCoefficients(seconds));
        return { result.first, result.second };
    }
    static std::array<double, 2> split(std::array<double, 2> state,
                                     float before, float after,
                                     double position, double seconds)
    {
        const auto result = YouKnowEngine::exactPwmHoldEndpoint(
            { state[0], state[1] }, before, true, position, after, seconds,
            YouKnowEngine::pwmHoldCoefficients(seconds));
        return { result.first, result.second };
    }
};
}

namespace
{
using namespace youknow;
using State = std::array<double, 2>;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// Independent fixture from the drawing, without reading production pole
// constants or recurrence: ideal opamp virtual-ground KCL, then output-node
// KCL. C62 is a feedback capacitor; C63 is grounded after the 47k resistor.
constexpr double feedback = 6.0 / (6.0 / 100000.0 + 15.0 / 560000.0);
State derivative(State state, double target)
{
    const double inputVolts = -100000.0 * (target / feedback - 15.0 / 560000.0);
    return { (-inputVolts / 100000.0 + 15.0 / 560000.0 - state[0] / feedback) / 47e-9,
             (state[0] - state[1]) / (47000.0 * 4.7e-9) };
}
State add(State a, State b, double scale)
{
    return { a[0] + scale * b[0], a[1] + scale * b[1] };
}
State circuit(State state, double target, double duration)
{
    const int steps = std::max(1, static_cast<int>(std::ceil(duration / 1e-6)));
    const double h = duration / steps;
    for (int i = 0; i < steps; ++i)
    {
        const auto a = derivative(state, target);
        const auto b = derivative(add(state, a, h / 2), target);
        const auto c = derivative(add(state, b, h / 2), target);
        const auto d = derivative(add(state, c, h), target);
        for (int node = 0; node < 2; ++node)
            state[node] += h * (a[node] + 2*b[node] + 2*c[node] + d[node]) / 6;
    }
    return state;
}
void near(State actual, State expected)
{
    // The engine's established pole representation is float; the numerical
    // nodal oracle and component values above use double. Bound that roundoff.
    require(std::abs(actual[0] - expected[0]) < 3e-7
                && std::abs(actual[1] - expected[1]) < 3e-7,
            "PWM control does not match independent capacitor KCL");
}

void circuitTests()
{
    require(std::abs(PwmControlCircuit::nominalFeedbackOhms - feedback) < 1e-9,
            "PWM trimmer no longer meets the nominal 50% service anchor");
    require(feedback >= 56000 && feedback <= 76000,
            "PWM feedback resistance exceeds the physical trimmer travel");
    require(std::abs(PwmControlCircuit::outputPoleSeconds - .0002209) < 1e-15,
            "PWM output pole must use R119, not the bias resistor R116");
    for (const State initial : { State{6,6}, State{-.8,-.8}, State{2.1,5.3} })
        for (float target : { -.8f, .6f, 3.2f, 6.0f })
            for (double seconds : { 1.0/768000, 1.0/48000, .0001, .001, .0042, .02 })
                near(YouKnowTestAccess::advance(initial,target,seconds),
                     circuit(initial,target,seconds));
    for (double position : {0.0,.173,.5,.891,1.0})
    {
        const State start{4.3,2.7};
        const double duration = .0003;
        auto expected = circuit(start,6.0,position*duration);
        expected = circuit(expected,-.8f,(1-position)*duration);
        near(YouKnowTestAccess::split(start,6.0f,-.8f,position,duration),expected);
    }
    for (float level : {-.8f,.6f,6.0f})
    {
        const State steady{level,level};
        near(YouKnowTestAccess::advance(steady,level,.0042),steady);
    }
}

std::vector<float> render(int block, int oversampling)
{
    YouKnowEngine engine;
    engine.prepare(48000.0, 128, oversampling);
    EngineParameters p;
    p.calibration = 0;
    p.aging = 0;
    p.pulseEnabled = true;
    p.sawEnabled = false;
    p.pwmSource = PwmSource::Manual;
    p.pwmDepth = 0;
    p.vcaMode = VcaMode::Gate;
    p.cutoff = 1;
    p.resonance = 0;
    p.chorus = ChorusMode::Off;
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    engine.setParameters(p);
    engine.noteOn(55,1);
    std::vector<float> left(24000),right(left.size());
    for (int start=0; start<24000;)
    {
        if (start==6000) { p.pwmDepth=101.0f/127; engine.setParameters(p); }
        if (start==12000) { p.pwmSource=PwmSource::Lfo; p.lfoRate=.85f; engine.setParameters(p); }
        if (start==18000) { p.pulseEnabled=false; engine.setParameters(p); }
        const int boundary = 6000*(start/6000+1);
        const int count = std::min(block,boundary-start);
        engine.process(left.data()+start,right.data()+start,count);
        start+=count;
    }
    for (float sample : left) require(std::isfinite(sample),"non-finite PWM audio");
    return left;
}
}

int main()
{
    try
    {
        circuitTests();
        for (int quality : {1,4})
            require(render(128,quality)==render(17,quality),
                    "PWM modulation, stepped controls or Pulse Off depend on block partition");
        std::cout << "PASS: PWM feedback/output capacitor KCL, physical VR31 bounds, DC preservation, fractional writes, shipping-render block invariance\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
