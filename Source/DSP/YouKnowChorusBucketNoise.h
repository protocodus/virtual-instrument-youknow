#pragma once
#include <algorithm>
#include <cmath>

namespace youknow
{
// Thornber1974, pp1236-1237/1244: transfer fluctuations enter successive
// packets with opposite signs; storage fluctuations are independent. Finite
// transfer weakens pair correlation. Reticon1977, p5/Fig6 gives the ideal
// sin^2(pi*f/fCP) spectrum. Neither supplies MN3009 microscopic strengths.
// https://vtda.org/pubs/BSTJ/vol53-1974/articles/bstj53-7-1211.pdf
// https://www.imagesensors.org/Past%20Workshops/Marvin%20White%20Collection/1977%20Short%20Course/1977%203%20Weckler.pdf#page=8
// This is an EFFECTIVE output-packet covariance family, not a per-bucket
// simulation, identified capacitance or physical transfer-efficiency estimate.
// x=sqrt(1-r)*s + sqrt(r/(1+c*c))*(t-c*t_previous), independent unit-variance
// s,t. Var(x)=1; normalized spectrum is 1-eta*cos(w), eta=2*r*c/(1+c*c).
// Only eta is identifiable from that PSD: r and c are not separate facts.
struct ChorusBucketNoise
{
    struct Coefficients
    {
        double storage { 1.0 };
        double transfer {};
        double correlation { 1.0 };
        double eta {};
        double aWeightedScale { 1.0 };
    };
    [[nodiscard]] static Coefficients coefficients(double fraction,
                                                    double correlation) noexcept
    {
        const double r=std::isfinite(fraction)?std::clamp(fraction,0.0,1.0):0.0;
        const double c=std::isfinite(correlation)?std::clamp(correlation,0.0,1.0):1.0;
        return {std::sqrt(1.0-r),std::sqrt(r/(1.0+c*c)),c,
                2.0*r*c/(1.0+c*c),1.0};
    }
    static void normalize(Coefficients& c,double weightedCosineMoment) noexcept
    {
        // Redistribute the ONE established output-noise budget, not add a
        // new floor. M=integral[A^2 |Hpost|^2 sinc^2(f/fCP) cos(w)] /
        // integral[A^2 |Hpost|^2 sinc^2(f/fCP)] over20Hz..20kHz. Thus this
        // scale preserves the existing A-weighted fixed-clock output power.
        // The initial unit-variance mix is a convention; after normalization
        // its packet variance changes. No microscopic kT/C value is inferred.
        const double m=std::isfinite(weightedCosineMoment)
            ? std::clamp(weightedCosineMoment,-1.0,1.0):0.0;
        c.aWeightedScale=1.0/std::sqrt(std::max(1.0-c.eta*m,1.0e-12));
    }
    [[nodiscard]] static float step(float storage,float transfer,
                                    float previousTransfer,const Coefficients& c) noexcept
    {
        if(c.transfer==0.0) return storage; // exact former source at r=0
        return static_cast<float>(c.aWeightedScale*(c.storage*storage
            +c.transfer*(transfer-c.correlation*previousTransfer)));
    }
};
}
