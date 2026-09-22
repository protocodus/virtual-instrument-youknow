#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"

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
// The idle chorus floor, where the line hiss is audible on its own.
std::vector<float> renderIdleChorus(EngineParameters parameters)
{
    YouKnowEngine engine;
    engine.prepare(48000.0, 128, 1);
    parameters.chorus = ChorusMode::One;
    engine.setParameters(parameters);
    std::vector<float> left(9600), right(left.size());
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    for (float value : left) require(std::isfinite(value), "non-finite chorus audio");
    return left;
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
        auto coreBand = nominal;
        coreBand.mainNoiseCalibrationProfile = MainNoiseCalibrationProfile::CoreBandTp8;
        auto coreScalar = nominal;
        coreScalar.mainNoiseLevelScale = 2.281f;
        require(render(coreBand, 128) == render(coreScalar, 128),
                "the core-band profile is not the chosen x2.281 source scale");

        // Hiss B multiplies Chorus Noise at the chorus, past the panel's 0..1
        // clamp: 0.25 under the profile is exactly the nominal 0.995.
        EngineParameters hissNominal;
        hissNominal.chorusNoise = 0.995f;
        auto hissProfile = hissNominal;
        hissProfile.chorusNoise = 0.25f;
        hissProfile.chorusNoiseCalibrationProfile = ChorusNoiseCalibrationProfile::IdleFloor439522;
        const auto hiss = renderIdleChorus(hissProfile);
        require(hiss == renderIdleChorus(hissNominal), "the hiss profile is not x3.98 on Chorus Noise");
        hissNominal.chorusNoise = 0.25f;
        require(hiss != renderIdleChorus(hissNominal), "the hiss profile was not connected to the chorus");
        require(hissNominal.chorusNoiseCalibrationProfile == ChorusNoiseCalibrationProfile::Nominal
                    && chorusNoiseCalibrationScale(static_cast<ChorusNoiseCalibrationProfile>(255)) == 1,
                "the hiss default or unknown-profile fallback changed");

        EngineParameters product;
        ProductFidelityProfile::applyTo(product);
        require(product.mainNoiseCalibrationProfile == MainNoiseCalibrationProfile::CoreBandTp8
                    && product.chorusNoiseCalibrationProfile
                           == ChorusNoiseCalibrationProfile::IdleFloor439522,
                "the product does not select the 2026-09-22 noise and hiss choices");
        nominal.noiseLevel = reference.noiseLevel = 0;
        nominal.sawEnabled = reference.sawEnabled = true;
        require(render(nominal, 128) == render(reference, 128), "noise calibration changed a noise-off tone");
        require(nominal.mainNoiseCalibrationProfile == MainNoiseCalibrationProfile::Nominal,
                "existing default changed");
        require(mainNoiseCalibrationScale(static_cast<MainNoiseCalibrationProfile>(255)) == 1,
                "unknown profile did not fall back to nominal");
        std::cout << "PASS: named profiles match their scalar signal paths; no noise-off change; block invariance; nominal fallback; product selections\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
