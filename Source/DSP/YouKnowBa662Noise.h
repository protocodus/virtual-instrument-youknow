#pragma once

#include <algorithm>

namespace youknow {
// Identified BA662 output mirrors, NOT an inferred IR3109 noise network.
// Open Music Labs reverse-engineered the original BA662 and identifies three
// Wilson output mirrors: IN+ is inverted once; IN- twice. Its October2015
// analysis pp4-5 gives the topology and branch orientation. The manufacturer's
// AS662 diagram independently draws it (p3); this corroborates topology, not
// original-device beta, flicker or installed noise measurements:
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
// No transistor beta/base-resistance/flicker excess or MHz pole is fitted.
struct Ba662Noise {
    static constexpr double electronCharge=1.602176634e-19;
    [[nodiscard]] static double outputMirrorCurrentPsd(double tailAmps,double y) noexcept {
        return 2*electronCharge*std::max(0.0,tailAmps)
            *(3-std::clamp(y,-1.0,1.0));
    }
};
}
