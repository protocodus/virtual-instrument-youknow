#pragma once

#include <array>

namespace youknow
{
// One serviced instrument, not original-80017A population calibration:
// https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16
// identifies #439522 with Borish replacement cards. Its owner confirms the
// 192kHz six-card sweep requested/uploaded in issue 44, comments
// 4359218903 and 4359681843:
// https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/44
// MIDI: https://kayrock.org/kr106/vcf_fullsweep.mid
// audio: https://www.lewisfrancis.com/nwio/vcf_fullsweep_bip.aif
//
// Fit three circuit coordinates per fixed physical slot: FREQ intercept,
// WIDTH slope, and effective control-current ceiling. The existing algebraic limiting
// exponent 1.7 and existing three DAC carry steps are held fixed. Alternating
// sorted measured codes train the fit; the complementary codes are held out.
// Values describe steady self-oscillation, with Unit Character zero in the
// model. The equivalent unsuppressed pole ceiling multiplies the stored
// limit-cycle ceiling by the fixed full-RES service trim (~1.12271), putting
// all six in 69.35..71.67kHz. The 64.8..72.9kHz fit bounds span historical
// 240/270pF model alternatives, not measured Borish tolerances; the 270pF
// alternative is clone evidence, NOT an original-80017A confidence bound. These effective parameters do not identify individual resistor,
// transistor or capacitor values. No audio-rate curve or point lookup is fit.
// One sweep per card confounds slot with time/direction; held-outs test
// interpolation, not a second recording. Reproduce with
// Tools/AnalyzeVoiceCardCalibration.py fit HARDWARE.json FIT.json
// --trim 1.1227132081985474 (shipping full-RES table value). Source audio SHA256:
// 27ab9ed0ce7c88c03eaf775216366abadc64df390b9107a54cc3ac35367956be
struct ReferenceVcfCalibration
{
    double baseHertz;
    double centsPerByte;
    double selfOscillationCeilingHertz;
};

inline constexpr std::array<ReferenceVcfCalibration, 6> serviced439522Vcf {{
    {5.8286002062009, 132.763033592879, 62588.4165795943},
    {5.99330705927156, 132.501795587154, 61961.3087916306},
    {5.93491445701664, 132.652467703567, 63437.306623427},
    {5.73976637342885, 133.278257128677, 61768.1837847212},
    {5.69732220120628, 133.634097706276, 62959.6674112299},
    {5.77514237526869, 133.379902865747, 63832.4632506023}
}};
} // namespace youknow
