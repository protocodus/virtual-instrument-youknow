#pragma once

#include <algorithm>

namespace youknow {
// Identified BA662 output mirrors, NOT an inferred IR3109 noise network.
// Open Music Labs' original-BA662 electrical reverse-engineering account
// identifies Wilson output mirrors (its text is reproduced by synthCube).
// Its October2015 analysis pp4-5 explains the common three-Wilson topology:
// IN+ is inverted once; IN- twice. Fig2 there is LM13700, not a BA662 die
// schematic. The manufacturer's AS662 diagram draws the matched topology
// (p3); this corroborates routing, not original-device beta, flicker or
// installed noise measurements:
// https://synthcube.com/open-music-labs-ba662-ota-clone/
// https://www.openmusiclabs.com/files/otadist.pdf
// https://www.ericasynths.lv/media/AS662D.pdf#page=3
// BA662F in the80017A has the same OTA as the SIP original (firsthand analysis):
// https://amsynths.co.uk/2018/01/07/all-about-the-ba662-chip/
//
// Matched high-beta/zero-Early-conductance limit, quasi-static audio band:
// a Wilson mirror's sense collector is forced by Iin. If ns and nd are the
// sense transistor and its diode's collector noises, KCL gives
// Iout=Iin-ns+nd. The third transistor's own collector source is cancelled by
// feedback through its free base node in this limit. Each surviving source
// has one-sided PSD2qI, so the mirror adds4qI, with the original branch source
// transferred once rather than counted again. Thus three mirrors add
// 4q*(Iplus+2*Iminus), Iplus/minus=Itail*(1+/-y)/2. y is the PHYSICAL
// differential pair's (Iplus-Iminus)/Itail, not an arbitrary feedback sign.
// This term is independent of the already generated quiet-tail pair term.
// AS662's lower-mirror series level-shift diode D2 adds no output source in
// this limit: its series current is forced and its shot source shifts the
// free sense-collector voltage. A ten-source/seven-node oracle includes it;
// the original BA662's presence/absence of that extra diode is not inferred.
// No transistor beta/base-resistance/flicker excess or MHz pole is fitted.
struct Ba662Noise {
    static constexpr double electronCharge=1.602176634e-19;
    static constexpr double boltzmann=1.380649e-23;
    // Reuse VoiceVcaSignalLaw's nominal500ohm mirror-emitter prior, attributed
    // there to Open Music Labs' original-BA662 measurement. This inherited
    // value is not a new component measurement; equal sides are an assumption.
    // AS662 p3 corroborates degenerating resistors but publishes400ohm(+/-20%),
    // not an original500ohm measurement.
    static constexpr double tailEmitterOhms=500.0;
    [[nodiscard]] static double outputMirrorCurrentPsd(double tailAmps,double y) noexcept {
        return 2*electronCharge*std::max(0.0,tailAmps)
            *(3-std::clamp(y,-1.0,1.0));
    }
    // The two-transistor tail mirror under an ideal quiet external control
    // current, matched high-beta/zero-Early-conductance limit. The forced
    // sense collector gives deltaVbe1=-n1/gm; its emitter resistor contributes
    // independent series voltage e1. The output transistor obeys
    // deltaItail=(-n1+n2+gm*(e1-e2))/(1+gm*R). Collector PSDs2qI and each
    // emitter resistor's4kTR therefore give the expression below. gm=I/Vt,
    // Vt=kT/q. The pair steers deltaItail to its output as y*deltaItail.
    // After unity mirror transfers this is y²*Stail, independent of both
    // the existing projected pair term and output-mirror collector terms.
    // No external Tr18/Tr20 noise, base/flicker or extra mirror factor is
    // inferred. The existing card temperature is a chassis proxy, not die T.
    [[nodiscard]] static double tailMirrorCurrentPsd(double tailAmps,double y,double kelvin) noexcept {
        const double current=std::max(0.0,tailAmps);
        const double gm=current*electronCharge/(boltzmann*std::max(1.0,kelvin));
        const double degeneration=1+gm*tailEmitterOhms;
        const double collector=4*electronCharge*current;
        const double resistors=8*boltzmann*std::max(1.0,kelvin)*tailEmitterOhms*gm*gm;
        const double steering=std::clamp(y,-1.0,1.0);
        return steering*steering*(collector+resistors)/(degeneration*degeneration);
    }
};
}
