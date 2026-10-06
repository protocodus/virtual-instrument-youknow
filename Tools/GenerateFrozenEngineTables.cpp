// This offline utility deliberately exposes private table data without adding
// generator hooks to the shipping DSP API. Include standard headers first.
#include "FrozenTableWriter.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <vector>
#if !defined(YOUKNOW_VERIFY_FROZEN_TABLES)
#undef YOUKNOW_EMBEDDED_TARGET
#endif
#define private public
#include "../Source/DSP/YouKnowEngine.cpp"
#undef private

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    using namespace youknow;
    using E = YouKnowEngine;
    using namespace frozen_table_writer;
    destination = argv[1];
    std::filesystem::create_directories(destination);
    scalar("CardJohnsonNoise", filterNoiseVoltsDerived);
    scalar("Describing", describingTable);
    std::array<float, 129> trim {};
    for (std::size_t i = 0; i < trim.size(); ++i)
        trim[i] = E::VoicedResonanceCompatibilityProfile::frequencyTrim(4.0f + i / 32.0f);
    scalar("ResonanceFrequencyTrim", trim);
    std::array<float, E::maxVoices> gradient {};
    for (int i = 0; i < E::maxVoices; ++i) gradient[i] = E::chassisGradientCelsius(i);
    scalar("ChassisGradient", gradient);
    scalar("ChassisGradientMean", E::chassisGradientMeanCelsius());
    scalar("VoiceVcaGain", E::VoiceVcaControlLaw::exactGainTable());
    scalar("VoiceVcaServiceGain", E::VoiceVcaSignalLaw::serviceGain());
    scalar("CorrectionStep", E::correctionTables().stepResponse);
    scalar("CorrectionSlope", E::correctionTables().slopeResidual);
    scalar("ResonanceJunction", E::CircuitDerivedResonanceProfile::junctionLoopGainTable());
    scalar("NoiseJunction", E::CircuitDerivedNoiseLevelProfile::junctionDriveTable());
    scalar("SubLevel", SubLevelDiodeLaw::table());
    const auto vcf = [](const char* name, const auto& table) {
        file(name, [&](auto& out) {
            out << "{{\n";
            for (const auto& c : table) {
                out << '{'; value(out, c.constant); out << ',';
                value(out, c.linear); out << ','; value(out, c.quadratic);
                out << ','; value(out, c.cubic); out << "},\n";
            }
            out << "}}";
        });
    };
    vcf("VcfTanhFine", vcfTanhFineTable);
    vcf("VcfTanhTail", vcfTanhTailTable);
    std::array<double, 4> gains { 1.0, 1.0, 1.0, 1.0 };
    const auto cycle = limitCycleFor(2.4, E::otaHeadroomVolts,
        E::VoicedResonanceCompatibilityProfile::loopHeadroomVolts, gains);
    file("NominalLimitCycle", [&](auto& out) {
        out << '{'; value(out, cycle.loopGain); out << ',';
        value(out, cycle.droop); out << '}';
    });
    const auto control = [](const char* name, const auto& circuit) {
        file(name, [&](auto& out) {
            out << '{'; value(out, circuit.charge_); out << ',';
            value(out, circuit.differential_); out << ',';
            value(out, circuit.kneeRegionEnd_); out << ',';
            value(out, circuit.kneeStep_); out << '}';
        });
    };
    control("VcaControl", E::voiceVcaControlCircuit(false));
    control("EvidenceVcaControl", E::voiceVcaControlCircuit(true));
    const auto& calibration = E::evidenceVcaCalibration();
    file("EvidenceVcaCalibration", [&](auto& out) {
        out << '{'; value(out, calibration.span_); out << ',';
        value(out, calibration.thermalVolts_); out << ',';
        value(out, calibration.referenceVbe_); out << ',';
        value(out, calibration.peakEmitterAmps_); out << ',';
        value(out, calibration.headroom_); out << ',';
        value(out, calibration.serviceGain_); out << ',';
        value(out, calibration.gain_); out << '}';
    });
    const auto& thermal = E::voiceVcaJunctionCircuit();
    file("VcaJunctionTemperature", [&](auto& out) {
        out << '{'; value(out, thermal.span_); out << ",{{\n";
        for (const auto& row : thermal.rows_) {
            out << '{'; value(out, row.capacitor); out << ',';
            value(out, row.control); out << ',';
            value(out, row.capacitorTemperatureSlope); out << ',';
            value(out, row.controlTemperatureSlope); out << "},\n";
        }
        out << "}}}";
    });
    FirmwareControlTrace::Tables firmware {};
    for (unsigned i = 0; i < 128; ++i) {
        firmware.attack[i] = attackIncrementForByte(static_cast<std::uint8_t>(i));
        firmware.portamento[i] = portamentoIncrementForIndex(static_cast<std::uint8_t>(i));
    }
    for (int i = 0; i < 104; ++i) {
        firmware.pitchCv[i] = derivedDcoCvAnchor(i);
        firmware.pitchDivider[i] = static_cast<std::uint16_t>(derivedDcoDividerAnchor(i));
    }
    file("FirmwareControl", [&](auto& out) {
        out << '{'; value(out, firmware.portamento); out << ',';
        value(out, firmware.attack); out << ',';
        value(out, firmware.pitchCv); out << ',';
        value(out, firmware.pitchDivider); out << '}';
    });
}
