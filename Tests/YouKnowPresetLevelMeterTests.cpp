#include "PresetLevelMeter.h"

#include <iostream>
#include <numeric>
#include <string>

namespace
{
using namespace youknow::preset_level_meter;

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

double responseDb(double frequency)
{
    const auto& h = referenceFilter().coefficients;
    long double real = 0.0L;
    for (std::size_t i = 0; i < h.size(); ++i)
        real += h[i] * std::cos(2.0L * std::numbers::pi_v<long double> * frequency
            * (static_cast<long double>(i) - 128.0L) / 96000.0L);
    return 20.0 * std::log10(std::max(1.0e-30, std::abs(static_cast<double>(real))));
}

void qualifyFilter()
{
    // Independent SciPy 1.17.0 oracle:
    // h = scipy.signal.firwin(257, 22000, window=('kaiser', 8.6),
    //                        fs=96000, scale=True)
    // Its dense 2^20-point freqz screen gives passband [-0.000185730,
    // +0.000149342] dB and worst stopband -92.962611 dB.
    const auto& h = referenceFilter().coefficients;
    require(h.size() == 257 && referenceFilter().groupDelayInputFrames == 128,
        "FIR support and delay changed");
    const std::array<std::pair<std::size_t, double>, 8> oracleTaps {{
        {0, 2.8697293942981343e-06}, {1, 2.6616749461978875e-06},
        {31, 0.00020469789350719667}, {64, -0.0014661573319093195},
        {96, 0.0066661391156023937}, {127, 0.31550767997313406},
        {128, 0.45833162038115421}, {256, 2.8697293942981343e-06}
    }};
    for (const auto& [index, value] : oracleTaps)
        require(std::abs(h[index] - value) < 2.0e-15, "FIR differs from SciPy coefficients");
    require(std::abs(std::accumulate(h.begin(), h.end(), 0.0) - 1.0) < 2.0e-15,
        "FIR DC gain changed");
    for (std::size_t i = 0; i < h.size(); ++i)
        require(std::abs(h[i] - h[h.size()-1-i]) < 1.0e-16, "FIR lost linear phase");
    double passError = 0.0, worstStop = -300.0;
    for (double frequency = 0; frequency <= 20000; frequency += 5)
        passError = std::max(passError, std::abs(responseDb(frequency)));
    for (double frequency = 24000; frequency <= 48000; frequency += 5)
        worstStop = std::max(worstStop, responseDb(frequency));
    require(passError < 0.0002, "0..20 kHz passband exceeds declared error");
    require(worstStop < -92.9, "24..48 kHz stopband misses declared rejection");
    require(responseDb(45600) < -130.0, "45.6 kHz resonance leaks through meter");
    std::cout << "FIR passband error " << passError << " dB, worst stopband "
              << worstStop << " dB\n";
}

void qualifyTimelineAndMixedSignal()
{
    // Independent scipy.signal.convolve(x, h, mode='same', method='direct')[::2].
    // Odd input length and both boundary samples exercise zero padding, flush,
    // decimation phase and delay compensation, rather than just settled tones.
    std::vector<double> input(4097);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = 0.2 + 0.6 * std::sin(2 * std::numbers::pi * 1000 * i / 96000)
            + 0.25 * std::sin(2 * std::numbers::pi * 19000 * i / 96000)
            + 0.1 * std::sin(2 * std::numbers::pi * 45600 * i / 96000);
    const auto output = to48k(input);
    require(output.size() == 2049, "Resampling shortened or extended the recording");
    const std::array<std::pair<std::size_t, double>, 14> oracleSamples {{
        {0, 0.24678540791586051}, {1, 0.40665161243021991},
        {2, 0.13286985794097844}, {3, 0.64385420434343932},
        {31, -0.02904839182773726}, {63, 0.65865436334351812},
        {64, 0.93611750150860173}, {65, 0.42814926354534177},
        {500, 0.37499887669977833}, {1000, -0.53611749154405697},
        {1984, 0.93611750150864181}, {2046, -0.35286575901234696},
        {2047, -0.10255262309832336}, {2048, -0.35323327149825429}
    }};
    for (const auto& [index, value] : oracleSamples)
        require(std::abs(output[index] - value) < 3.0e-12,
            "Centered mixed-signal output differs from SciPy oracle");
    double sum = 0.0, squares = 0.0;
    for (double value : output) { sum += value; squares += value * value; }
    require(std::abs(sum / output.size() - 0.20328510711603717) < 3.0e-13,
        "Mixed-signal mean differs from independent oracle");
    require(std::abs(std::sqrt(squares / output.size()) - 0.5021854270764794) < 3.0e-13,
        "Mixed-signal RMS differs from independent oracle");

    std::fill(input.begin(), input.end(), 0.0);
    input[1024] = 1.0;
    const auto impulse = to48k(input);
    sum = 0.0;
    double moment = 0.0;
    for (std::size_t i = 0; i < impulse.size(); ++i)
    { sum += impulse[i]; moment += i * impulse[i]; }
    require(std::abs(moment / sum - 512.0) < 1.0e-10,
        "FIR delay shifted the event timeline");
}

void qualifyMeasurementAndNativeDiagnostics()
{
    for (double rate : {44100.0, 48000.0})
    {
        // Seven 400 ms windows contain energies 0,.25,.5,.75,1,1,1.
        // The zero window is gated out: expected mean-square = 4.5/6 = .75.
        StereoMeter meter(rate);
        for (int i = 0; i < static_cast<int>(rate); ++i)
        {
            const double value = i < static_cast<int>(rate * 0.4) ? 0.0 : 1.0;
            meter.add(value, value);
        }
        const auto result = meter.finish();
        require(std::abs(result.gatedRmsDb - 10 * std::log10(0.75)) < 1.0e-13,
            "Native relative-gate/window arithmetic changed");
        require(result.gatedRmsDb == result.nativeGatedRmsDb
            && result.nativePeakDb == 0 && result.measurementSampleRate == rate
            && result.measurementFrames == static_cast<std::size_t>(rate),
            "44.1/48 kHz native metrics or score length changed");
    }

    StereoMeter meter(96000.0);
    double peak = 0.0;
    for (int i = 0; i < 96000; ++i)
    {
        const double sample = 0.1 * std::sin(2 * std::numbers::pi * 1000 * i / 96000)
            + std::sin(2 * std::numbers::pi * 45600 * i / 96000);
        meter.add(sample, sample);
        peak = std::max(peak, std::abs(sample));
    }
    const auto result = meter.finish();
    require(result.measurementSampleRate == 48000 && result.measurementFrames == 48000,
        "96 kHz measurement grid or duration is wrong");
    require(std::abs(result.gatedRmsDb - 10 * std::log10(0.005)) < 0.005,
        "Ultrasonic energy contaminated the reference-grid level");
    require(std::abs(result.nativeGatedRmsDb - 10 * std::log10(0.505)) < 1.0e-10,
        "Native full-band energy was hidden");
    require(result.nativePeakDb == decibels(peak) && result.nativePeakDb > 0.0,
        "Measurement filtering hid the native clipping peak");
    std::cout << "Mixed-tone reference/native RMS " << result.gatedRmsDb << "/"
              << result.nativeGatedRmsDb << " dBFS; native peak "
              << result.nativePeakDb << " dBFS\n";
}
}

int main()
{
    try
    {
        qualifyFilter();
        qualifyTimelineAndMixedSignal();
        qualifyMeasurementAndNativeDiagnostics();
        std::cout << "Preset level meter qualification passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
