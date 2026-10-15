// The physical reference switches Tr19 only between its two circuit states.
// It integrates C56's continuous dV/dt=I/C with RK4, split at every switch
// edge, and integrates Fourier coefficients before sampling. The existing
// independently KCL-tested static diode/node solve is shared, but neither its
// trapezoidal capacitor companion nor the new reconstruction is used here.
// This isolates numerical switch reconstruction from unknown hardware values.
#include "../Source/DSP/YouKnowCoupledMixer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace
{
using Mixer = youknow::CoupledSubMixer;
using Complex = std::complex<double>;
constexpr double pi = 3.14159265358979323846;
constexpr int harmonicCount = 40;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

double sourceAt(const Mixer::Calibration& c, double phase, bool mixed)
{
    const double dc = c.sourceBiasVolts + c.sourceScale
        * c.oscillatorDriveScale * 6.0 * c.pulseSourceScale;
    // The moving source's physical voltage span equals the established
    // unipolar saw span. A sine isolates mixing without introducing another
    // source reconstruction's aliases into the switch-only measurement.
    return dc + (mixed ? c.sourceScale * c.oscillatorDriveScale
        * 6.0 * (1.0 + std::sin(2.0 * phase)) : 0.0);
}

struct ContinuousReference
{
    Mixer::Calibration c { Mixer::evidenceCalibration() };
    double frequency {};
    bool mixed {};
    int steps {};

    double current(double capacitor, double phase, double gate) const
    {
        return Mixer::solve(c, sourceAt(c, phase, mixed),
            Mixer::railFullScaleVolts, gate, 1.0 / c.loadOhms,
            capacitor).capacitorAmps;
    }

    double period(double capacitor,
        std::array<Complex, harmonicCount + 1>* spectrum = nullptr) const
    {
        const double seconds = 1.0 / (frequency * steps);
        const double phaseStep = 2.0 * pi / steps;
        for (int sample = 0; sample < steps; ++sample)
        {
            const double phase = sample * phaseStep;
            const double gate = sample < steps / 2 ? 1.0 : 0.0;
            const double first = current(capacitor, phase, gate);
            const double k1 = first / Mixer::couplingFarads;
            const double k2 = current(capacitor + 0.5 * seconds * k1,
                phase + 0.5 * phaseStep, gate) / Mixer::couplingFarads;
            const double k3 = current(capacitor + 0.5 * seconds * k2,
                phase + 0.5 * phaseStep, gate) / Mixer::couplingFarads;
            const double k4 = current(capacitor + seconds * k3,
                phase + phaseStep, gate) / Mixer::couplingFarads;
            capacitor += seconds * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
            if (spectrum != nullptr)
            {
                const double last = current(capacitor, phase + phaseStep, gate);
                const double scale = c.loadOhms * c.pinToCoreGain / (2.0 * steps);
                for (int h = 0; h <= harmonicCount; ++h)
                    (*spectrum)[static_cast<std::size_t>(h)] += scale
                        * (first * std::polar(1.0, -h * phase)
                           + last * std::polar(1.0, -h * (phase + phaseStep)));
            }
        }
        return capacitor;
    }

    std::array<Complex, harmonicCount + 1> spectrum() const
    {
        // Shoot for the periodic capacitor state; long run-in is unnecessary
        // and cannot conceal an initial-state mismatch in the harmonic test.
        double a = 0.0, b = 2.0;
        double fa = period(a) - a, fb = period(b) - b;
        for (int iteration = 0; iteration < 12; ++iteration)
        {
            const double next = b - fb * (b - a) / (fb - fa);
            a = b;
            fa = fb;
            b = next;
            fb = period(b) - b;
            if (std::abs(fb) < 1.0e-13)
                break;
        }
        require(std::abs(fb) < 1.0e-11, "continuous C56 periodic solve failed");
        std::array<Complex, harmonicCount + 1> result {};
        period(b, &result);
        return result;
    }
};

std::vector<double> render(int rate, int frequency, bool mixed, bool corrected)
{
    const auto c = Mixer::evidenceCalibration();
    const auto coefficients = Mixer::prepareCoefficients(c, 1.0 / rate);
    const int repeat = rate / std::gcd(rate, frequency);
    std::vector<double> gate(static_cast<std::size_t>(repeat));
    std::vector<double> source(static_cast<std::size_t>(repeat));
    for (int sample = 0; sample < repeat; ++sample)
    {
        const double phase = 2.0 * pi * frequency * sample / rate;
        double weight = 0.5;
        for (int h = 1; h * frequency < 0.49 * rate; h += 2)
            weight += 2.0 * std::sin(h * phase) / (pi * h);
        gate[static_cast<std::size_t>(sample)] = weight;
        source[static_cast<std::size_t>(sample)] = sourceAt(c, phase, mixed);
    }
    Mixer mixer;
    std::vector<double> output(static_cast<std::size_t>(rate));
    for (int sample = -rate; sample < rate; ++sample)
    {
        const auto phase = static_cast<std::size_t>((sample + rate) % repeat);
        const auto result = corrected
            ? mixer.processBandlimitedSwitch(c, coefficients, source[phase],
                Mixer::railFullScaleVolts, gate[phase])
            : mixer.process(c, coefficients, source[phase],
                Mixer::railFullScaleVolts, gate[phase]);
        if (sample >= 0)
            output[static_cast<std::size_t>(sample)] = result.filterVolts * c.pinToCoreGain;
    }
    return output;
}

Complex projection(const std::vector<double>& samples, double frequency, int rate)
{
    Complex result {};
    const auto step = std::polar(1.0, -2.0 * pi * frequency / rate);
    Complex phase { 1.0, 0.0 };
    for (double sample : samples)
    {
        result += sample * phase;
        phase *= step;
    }
    return result / static_cast<double>(samples.size());
}

void physicalReferenceAndAliases()
{
    for (int frequency : { 500, 1760, 3500 })
        for (bool mixed : { false, true })
        {
            auto reference = ContinuousReference {
                Mixer::evidenceCalibration(), static_cast<double>(frequency), mixed, 8192
            };
            const auto fine = reference.spectrum();
            reference.steps = 4096;
            const auto coarse = reference.spectrum();
            double refinement = 0.0, signal = 0.0;
            for (int h = 1; h <= harmonicCount && h * frequency < 20000; ++h)
            {
                refinement += std::norm(fine[h] - coarse[h]);
                signal += std::norm(fine[h]);
            }
            require(std::sqrt(refinement / signal) < 2.0e-5,
                "continuous switch reference did not converge");
            for (int rate : { 48000, 96000 })
            {
                const auto before = render(rate, frequency, mixed, false);
                const auto after = render(rate, frequency, mixed, true);
                double oldError = 0.0, newError = 0.0;
                for (int h = 1; h <= harmonicCount && h * frequency < 20000; ++h)
                {
                    oldError += std::norm(projection(before, h * frequency, rate) - fine[h]);
                    newError += std::norm(projection(after, h * frequency, rate) - fine[h]);
                }
                const double oldDb = 10.0 * std::log10(oldError / signal);
                const double newDb = 10.0 * std::log10(newError / signal);
                require(newError < 0.01 * oldError,
                    "physical switch harmonic error did not improve by 20 dB");
                require(newDb < (mixed ? -55.0 : -85.0),
                    "switch reconstruction disagrees with physical C56 reference");
                std::cout << "switch " << frequency << " Hz mixed=" << mixed
                          << " rate=" << rate << " wanted error " << oldDb
                          << " -> " << newDb << " dBr\n";

                // 1760 Hz is A6, reachable with MIDI 93 in 4' range; 3500
                // is explicitly an extended-range stress. Folded SUB
                // harmonics are checked only when they are not real lines.
                if (!mixed && frequency != 500)
                {
                    double oldAlias = 0.0, newAlias = 0.0;
                    for (int h = 1; h <= 400; ++h)
                    {
                        if (h * frequency <= rate / 2)
                            continue;
                        const int wrapped = (h * frequency) % rate;
                        const int alias = std::min(wrapped, rate - wrapped);
                        if (alias < 100 || alias > 20000 || alias % frequency == 0)
                            continue;
                        oldAlias = std::max(oldAlias, std::abs(projection(before, alias, rate)));
                        newAlias = std::max(newAlias, std::abs(projection(after, alias, rate)));
                    }
                    const double oldAliasDb = 20.0 * std::log10(oldAlias / std::abs(fine[1]));
                    const double newAliasDb = 20.0 * std::log10(newAlias / std::abs(fine[1]));
                    require(newAlias < 0.001 * oldAlias,
                        "nonharmonic switch aliases did not improve by 60 dB");
                    require(newAliasDb < -100.0, "sub switch alias floor remains excessive");
                    std::cout << "  folded line " << oldAliasDb << " -> " << newAliasDb << " dBc\n";
                }
            }
        }
}

void integerStatesAndLifecycle()
{
    const auto c = Mixer::evidenceCalibration();
    Mixer before, after;
    before.prime(c, 0.4, 5.0, 0.5);
    after.prime(c, 0.4, 5.0, 0.5);
    for (int rate : { 8000, 44100, 48000, 192000 })
    {
        const auto coefficients = Mixer::prepareCoefficients(c, 1.0 / rate);
        for (int sample = 0; sample < 500; ++sample)
        {
            const double source = 2.0 * std::sin(sample * 0.037);
            const double rail = Mixer::railFullScaleVolts * sample / 500.0;
            const double gate = (sample / 17) % 2;
            const auto a = before.process(c, coefficients, source, rail, gate);
            const auto b = after.processBandlimitedSwitch(c, coefficients, source, rail, gate);
            require(a.waveVolts == b.waveVolts && a.filterVolts == b.filterVolts
                && a.capacitorAmps == b.capacitorAmps && a.subOffAmps == b.subOffAmps
                && a.subOnAmps == b.subOnAmps
                && before.capacitorVolts() == after.capacitorVolts(),
                "integer switch changed the physical circuit or retained capacitor");
        }
    }
    after.reset();
    require(after.capacitorVolts() == 0.0 && after.capacitorAmps() == 0.0,
        "corrected switch failed to reset");
}
}

int main()
{
    try
    {
        integerStatesAndLifecycle();
        physicalReferenceAndAliases();
        std::cout << "Coupled mixer bandlimited switch tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
