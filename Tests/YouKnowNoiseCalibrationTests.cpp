#include "DSP/YouKnowEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
using namespace youknow;
void require(bool pass, const char* why)
{
    if (!pass) throw std::runtime_error(why);
}
std::vector<float> render(EngineParameters parameters, int block)
{
    YouKnowEngine engine;
    engine.prepare(48000.0, 128, 1);
    parameters.vcfTanhMode = VcfTanhMode::PolyZoned;
    parameters.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    parameters.vcfSolverMode = VcfSolverMode::Rk4Single;
    parameters.chorus = ChorusMode::Off;
    parameters.chorusNoise = 0;
    engine.setParameters(parameters);
    for (int note : { 48, 55, 60 }) engine.noteOn(note, 1.0f);
    std::vector<float> result(9600), right(result.size());
    for (int frame = 0; frame < static_cast<int>(result.size());)
    {
        const int count = std::min(block, static_cast<int>(result.size()) - frame);
        engine.process(result.data() + frame, right.data() + frame, count);
        frame += count;
    }
    for (float value : result) require(std::isfinite(value), "non-finite profile audio");
    return result;
}
}
int main()
{
    try
    {
        EngineParameters nominal;
        nominal.noiseLevel = 1;
        nominal.sawEnabled = false;
        nominal.pulseEnabled = false;
        nominal.cutoff = .38f;
        nominal.resonance = .85f;
        auto reference = nominal;
        reference.mainNoiseCalibrationProfile = MainNoiseCalibrationProfile::Serviced439522;
        auto scalar = nominal;
        scalar.mainNoiseLevelScale = mainNoiseCalibrationScale(reference.mainNoiseCalibrationProfile);
        const auto named = render(reference, 128);
        require(named == render(scalar, 128), "profile is not the fitted pre-filter source scale");
        require(named == render(reference, 1), "noise profile depends on host block size");
        require(named != render(nominal, 128), "reference profile was not connected to the noise path");
        nominal.noiseLevel = reference.noiseLevel = 0;
        nominal.sawEnabled = reference.sawEnabled = true;
        require(render(nominal, 128) == render(reference, 128), "noise calibration changed a noise-off tone");
        require(nominal.mainNoiseCalibrationProfile == MainNoiseCalibrationProfile::Nominal,
                "existing default changed");
        require(mainNoiseCalibrationScale(static_cast<MainNoiseCalibrationProfile>(255)) == 1,
                "unknown profile did not fall back to nominal");
        std::cout << "PASS: named profile matches actual fitted scalar signal path; no noise-off change; block invariance; nominal fallback\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
