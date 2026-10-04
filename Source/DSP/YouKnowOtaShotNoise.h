#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

namespace youknow {
// Collector noise of the declared bare bipolar differential pairs. This is
// NOT a fitted full-IC floor: original BA662/IR3109 mirror, base-resistance
// and flicker spectra remain unknown. For each transistor S_Ic=2*q*Ic;
// a noiseless shared tail projects the common component out. With
// y=(Ic1-Ic2)/Itail=tanh(Vdiff/(2*Vt)), differential output PSD is
// 2*q*Itail*(1-y*y). At balance this is 2*q*Itail, or input PSD4*k*T/gm.
// General transistor collector/input noise, TI AN-222 pp3-4:
// https://www.ti.com/lit/pdf/snoa626
// Schottky density and separation from external resistor noise, ADI MT-047 p2:
// https://www.analog.com/media/en/training-seminars/tutorials/MT-047.pdf
// Family pair/tail/current-mirror topology and gm=Ic/(2*Vt), Rohm pp3-4:
// https://experimentalistsanonymous.com/diy/Datasheets/BA6110.pdf
// Large-drive reduction is ONLY this quiet-tail collector contribution; a
// real noisy tail/mirror can supply other terms. No excess factor is invented.
struct OtaShotNoise {
    static constexpr double electronCharge=1.602176634e-19;
    using Vector=std::array<double,4>;
    using Matrix=std::array<Vector,4>;
    [[nodiscard]] static double currentPsd(double tailAmps,double pairOutput) noexcept {
        return 2*electronCharge*std::max(0.0,tailAmps)
            * std::max(0.0,1-pairOutput*pairOutput);
    }
    struct Random {
        std::uint32_t state {1}; double spare {}; bool hasSpare {};
        void seed(std::uint32_t value) noexcept {state=value|1u;spare=0;hasSpare=false;}
        [[nodiscard]] double uniform() noexcept {
            state^=state<<13;state^=state>>17;state^=state<<5;
            return (static_cast<double>(state)+.5)/4294967296.0;
        }
        [[nodiscard]] double next() noexcept {
            if(hasSpare){hasSpare=false;return spare;}
            // Marsaglia & Bray, SIAM Review6(3),1964,pp260-264:
            // https://doi.org/10.1137/1006063
            // Four bounded polar attempts avoid sin/cos in almost all pairs.
            // If all are rejected, fresh independent Box--Muller uniforms
            // supply the same Gaussian law. The fallback is NOT a clipped
            // or recycled rejection point; worst callback work is bounded.
            for(int attempt=0;attempt<4;++attempt) {
                const double a=2*uniform()-1,b=2*uniform()-1,s=a*a+b*b;
                if(s>0&&s<1) {
                    const double scale=std::sqrt(-2*std::log(s)/s);
                    spare=b*scale;hasSpare=true;return a*scale;
                }
            }
            const double r=std::sqrt(-2*std::log(uniform()));
            const double phase=2*std::numbers::pi*uniform();
            spare=r*std::sin(phase);hasSpare=true;return r*std::cos(phase);
        }
    };
    [[nodiscard]] static Matrix multiply(const Matrix&a,const Matrix&b) noexcept {
        Matrix c{};
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)
            for(std::size_t k=0;k<4;++k)c[i][j]+=a[i][k]*b[k][j];
        return c;
    }
    [[nodiscard]] static Matrix transpose(const Matrix&a) noexcept {
        Matrix b{};for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)b[i][j]=a[j][i];return b;
    }
    // Locally frozen linear SDE dV=J*Vdt+B*dW. Single-sided current PSD S
    // gives BB'=S/(2*C*C); thus dW has variance dt, NOT fs/2 or one fresh
    // draw per RK evaluation. The covariance of a physical interval is
    // Q=integral exp(Jt) BB' exp(J't)dt. Integrate it using the Lyapunov
    // power series at ||Jdt||inf<=.25, then exact covariance doubling.
    // The unstructured eight-term reference retains high numerical accuracy.
    // This is a numerical accuracy convention, not a physical noise pole.
    // It is exact for the frozen linear circuit; freezing its midpoint
    // Jacobian/diffusion during a nonlinear interval is an explicit local
    // linearization approximation. The normal filter retains its own solver.
    [[nodiscard]] static Matrix covariance(Matrix a,Vector intervalDiffusion) noexcept {
        double norm=0;for(const auto&row:a){double sum=0;for(double x:row)sum+=std::abs(x);norm=std::max(norm,sum);}
        int squares=0;while(norm>.25&&squares<12){norm*=.5;++squares;}
        const double scale=std::ldexp(1.0,-squares);
        for(auto&row:a)for(double&x:row)x*=scale;
        Matrix f{},term{},q{};
        for(std::size_t i=0;i<4;++i){f[i][i]=term[i][i]=1;q[i][i]=intervalDiffusion[i]*scale;}
        for(int order=1;order<=8;++order){term=multiply(a,term);for(auto&row:term)for(double&x:row)x/=order;
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)f[i][j]+=term[i][j];}
        term=q;const auto at=transpose(a);
        for(int order=1;order<=8;++order){const auto left=multiply(a,term),right=multiply(term,at);
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j){term[i][j]=(left[i][j]+right[i][j])/(order+1);q[i][j]+=term[i][j];}}
        for(int square=0;square<squares;++square){const auto propagated=multiply(multiply(f,q),transpose(f));
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)q[i][j]+=propagated[i][j];f=multiply(f,f);}
        // Remove roundoff asymmetry before Cholesky, without changing energy.
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[i][j]=q[j][i]=.5*(q[i][j]+q[j][i]);
        return q;
    }
    // The four-stage ring has only its diagonal, three lower neighbours
    // and the stage-four resonance return. Exploit those eight entries in
    // the SAME series; exp(A) is unnecessary when no doubling is required.
    // This sparse solve itself reads the current interval without quantization;
    // Cache below separately reuses its factor within declared error tolerances.
    [[nodiscard]] static Matrix cascadeCovariance(const Matrix&a,const Vector&intervalDiffusion) noexcept {
        const auto [minimum,maximum]=std::minmax_element(intervalDiffusion.begin(),intervalDiffusion.end());
        // Truncating a Lyapunov series need not preserve PSD when only one
        // pair conducts. Then a tiny downstream diagonal can be lost even
        // though the dominant-node Frobenius error is tiny. Use a positive
        // Gram quadrature for >100:1 diffusion imbalance or saturated pairs.
        if(*minimum<.01 * *maximum)return positiveCovariance(a,intervalDiffusion);
        Vector diagonal{},neighbour{};
        double norm=0;
        for(std::size_t i=0;i<4;++i) {
            diagonal[i]=a[i][i];neighbour[i]=a[i][(i+3)&3];
            norm=std::max(norm,std::abs(diagonal[i])+std::abs(neighbour[i]));
        }
        int squares=0;while(norm>.25&&squares<12){norm*=.5;++squares;}
        const double scale=std::ldexp(1.0,-squares);
        Matrix q{},term{};
        // Relative covariance accuracy is screened at 1e-5 against an
        // independent continuous ODE oracle. The physical model does not
        // warrant doing eight terms for every very small interval.
        const int terms=norm<=.125?4:6;
        for(std::size_t i=0;i<4;++i) {
            diagonal[i]*=scale;neighbour[i]*=scale;
            q[i][i]=term[i][i]=intervalDiffusion[i]*scale;
        }
        for(int order=1;order<=terms;++order) {
            Matrix next{};
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<=i;++j) {
                const double value=((diagonal[i]+diagonal[j])*term[i][j]
                    +neighbour[i]*term[(i+3)&3][j]
                    +neighbour[j]*term[i][(j+3)&3])/(order+1);
                next[i][j]=next[j][i]=value;q[i][j]+=value;
                if(i!=j)q[j][i]+=value;
            }
            term=next;
        }
        if(squares>0) {
            Matrix f{};term={};
            for(std::size_t i=0;i<4;++i)f[i][i]=term[i][i]=1;
            for(int order=1;order<=terms;++order) {
                Matrix next{};
                for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j) {
                    next[i][j]=(diagonal[i]*term[i][j]
                        +neighbour[i]*term[(i+3)&3][j])/order;
                    f[i][j]+=next[i][j];
                }
                term=next;
            }
            for(int square=0;square<squares;++square) {
                const auto propagated=multiply(multiply(f,q),transpose(f));
                for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)q[i][j]+=propagated[i][j];
                f=multiply(f,f);
            }
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[i][j]=q[j][i]=.5*(q[i][j]+q[j][i]);
        }
        return q;
    }
    // Positive quadrature of the same four-stage ring (not an arbitrary J).
    [[nodiscard]] static Matrix positiveCovariance(Matrix a,Vector d) noexcept {
        double norm=0;for(const auto&row:a){double sum=0;for(double x:row)sum+=std::abs(x);norm=std::max(norm,sum);}
        int squares=0;while(norm>.25&&squares<12){norm*=.5;++squares;}
        const double scale=std::ldexp(1.0,-squares);
        for(auto&row:a)for(double&x:row)x*=scale;for(double&x:d)x*=scale;
        constexpr Vector nodes{.06943184420297371,.33000947820757187,.6699905217924281,.9305681557970262};
        constexpr Vector weights{.1739274225687269,.3260725774312731,.3260725774312731,.1739274225687269};
        const auto exponential=[&](double position,int terms) {
            Matrix f{},term{};for(std::size_t i=0;i<4;++i)f[i][i]=term[i][i]=1;
            for(int order=1;order<=terms;++order){Matrix next{};
                for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j) {
                    next[i][j]=(a[i][i]*term[i][j]+a[i][(i+3)&3]*term[(i+3)&3][j])*position/order;
                    f[i][j]+=next[i][j];
                }
                term=next;
            }return f;
        };
        // Positive four-node Gauss--Legendre integration of F(t)D F(t)':
        // four nodes retain all four controllable directions even for one
        // source. exp Taylor8 is used on this <=.25 scaled interval;
        // projected node variances are screened independently, not only norm.
        Matrix q{};
        for(std::size_t node=0;node<4;++node) {
            const auto f=exponential(nodes[node],8);
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<=i;++j) {
                double value=0;for(std::size_t k=0;k<4;++k)value+=f[i][k]*d[k]*f[j][k];
                q[i][j]+=weights[node]*value;
            }
        }
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[j][i]=q[i][j];
        if(squares>0){auto f=exponential(1,8);
            for(int square=0;square<squares;++square){const auto propagated=multiply(multiply(f,q),transpose(f));
                for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<4;++j)q[i][j]+=propagated[i][j];f=multiply(f,f);}
            for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<i;++j)q[i][j]=q[j][i]=.5*(q[i][j]+q[j][i]);
        }return q;
    }
    [[nodiscard]] static Matrix factor(const Matrix&q,bool*valid=nullptr) noexcept {
        if(valid)*valid=true;
        Matrix l{};
        for(std::size_t i=0;i<4;++i){for(std::size_t j=0;j<=i;++j){double value=q[i][j];
            for(std::size_t k=0;k<j;++k)value-=l[i][k]*l[j][k];
            // Only roundoff-scale negative pivots may be floored. Cache
            // retries a non-roundoff failure through positive quadrature.
            if(i==j&&value < -256*std::numeric_limits<double>::epsilon()*std::abs(q[i][i])) {
                if(valid)*valid=false;return {};
            }
            if(i!=j&&l[j][j]==0&&value!=0){if(valid)*valid=false;return {};}
            l[i][j]=i==j?std::sqrt(std::max(0.0,value)):l[j][j]>0?value/l[j][j]:0;}}
        return l;
    }
    [[nodiscard]] static Vector applyFactor(const Matrix&l,const Vector&normal) noexcept {
        Vector result{};
        for(std::size_t i=0;i<4;++i)for(std::size_t j=0;j<=i;++j)result[i]+=l[i][j]*normal[j];
        return result;
    }
    [[nodiscard]] static Vector innovation(const Matrix&q,const Vector&normal) noexcept {
        return applyFactor(factor(q),normal);
    }
    struct Cache {
        Matrix a{},l{};Vector d{};bool primed{};
        // Numerical reuse, not a physical control-rate hold: compare the
        // dimensionless interval Jacobian and every node's diffusion each
        // call. No noise draws are skipped. Independent covariance screens
        // find reuse errors below .3% (0.013 dB of power) in tested cases;
        // this is not a universal bound for all possible nonlinear histories.
        static constexpr double jacobianTolerance=.00025;
        static constexpr double diffusionTolerance=.001;
        [[nodiscard]] Vector next(const Matrix&currentA,const Vector&currentD,const Vector&normal) noexcept {
            bool rebuild=!primed;
            for(std::size_t i=0;i<4&&!rebuild;++i) {
                double difference=0;
                for(std::size_t j=0;j<4;++j)difference+=std::abs(currentA[i][j]-a[i][j]);
                rebuild=difference>jacobianTolerance
                    ||std::abs(currentD[i]-d[i])>diffusionTolerance*std::max(currentD[i],d[i]);
            }
            if(rebuild) {a=currentA;d=currentD;bool valid;
                l=factor(cascadeCovariance(a,d),&valid);
                if(!valid)l=factor(positiveCovariance(a,d));
                primed=true;}
            return applyFactor(l,normal);
        }
    };
};
}
