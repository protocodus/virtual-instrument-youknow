// IC3 finite-resistance comparison, not an installed-part calibration.
// Independent complex component-node MNA qualifies each selected transfer;
// an independent capacitor-voltage/current DAE plus fine RK4 checks charge
// redistribution. The oracle does not use the production midpoint matrix.
// --render creates shipping-path A/B/C (0/110/240 ohms), frozen baseline A
// can be built with YOUKNOW_HPF_BASELINE against e405d7a headers/library.
// To isolate the reviewed switch from later engine changes, build e41c573
// with the reviewed HighPassSwitch header and this audit overlaid, then
// require its raw A to match that independently built frozen baseline.
// 110/240 ohms are Toshiba's 10V/5V typical table points, NOT a measured
// bound for the Juno's +5V/Tr3 rail or its signal-dependent switch resistance.
#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"

#include <complex>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>

#ifndef YOUKNOW_HPF_BASELINE
namespace youknow
{
struct HighPassSwitchTestAccess
{
    using State = std::array<double, 6>;
    static void seed(HighPassSwitchCircuit& circuit, const State& state, int mode)
    {
        std::copy_n(state.begin(), 5, circuit.voltage_.begin());
        circuit.feedbackVoltage_ = state[5]; circuit.previousMode_ = mode;
        circuit.switchRemainingSeconds_ = 0;
    }
    static State state(const HighPassSwitchCircuit& circuit)
    {
        State result;
        std::copy_n(circuit.voltage_.begin(), 5, result.begin());
        result[5] = circuit.feedbackVoltage_; return result;
    }
    static double remainingSeconds(const HighPassSwitchCircuit& circuit)
    { return circuit.switchRemainingSeconds_; }
    static double windowSeconds(const HighPassSwitchCircuit& circuit)
    { return circuit.switchWindowSeconds_; }
};
}
#endif

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
StereoBuffer render(double ron, int block=128);
using Complex = std::complex<long double>;
template<class T, std::size_t N> using Matrix = std::array<std::array<T,N>,N>;
template<class T, std::size_t N>
std::array<T,N> solve(Matrix<T,N> a, std::array<T,N> b)
{
    for (std::size_t col=0;col<N;++col)
    {
        std::size_t pivot=col;
        for(std::size_t row=col+1;row<N;++row)
            if(std::abs(a[row][col])>std::abs(a[pivot][col]))pivot=row;
        if(std::abs(a[pivot][col])<1e-24L)throw std::runtime_error("singular reference");
        std::swap(a[col],a[pivot]);std::swap(b[col],b[pivot]);
        for(std::size_t row=col+1;row<N;++row)
        {
            const T r=a[row][col]/a[col][col];
            for(std::size_t j=col;j<N;++j)a[row][j]-=r*a[col][j];
            b[row]-=r*b[col];
        }
    }
    std::array<T,N> x {};
    for(int row=static_cast<int>(N)-1;row>=0;--row)
    {
        T v=b[row];for(std::size_t j=row+1;j<N;++j)v-=a[row][j]*x[j];
        x[row]=v/a[row][row];
    }
    return x;
}
template<class T,std::size_t N> void edge(Matrix<T,N>& a,int p,int q,T g)
{
    if(p>=0)a[p][p]+=g;
    if(q>=0)a[q][q]+=g;
    if(p>=0&&q>=0){a[p][q]-=g;a[q][p]-=g;}
}
template<class T,std::size_t N> void resistors(Matrix<T,N>& a,int mode,long double ron)
{
    // Literal nodes: C14 right, Y0, C11 right, Y1, C10 right, Y2, Y3, N.
    edge(a,0,-1,T(1.L/33000));edge(a,1,-1,T(1.L/1e6));edge(a,3,-1,T(1.L/1e6));
    for(int n:{2,4,5,6})edge(a,n,-1,T(1.L/47000));
    edge(a,6,7,T(1.L/47000));
    const int y=mode==0?6:mode==1?5:mode==2?3:1;
    edge(a,0,y,T(1.L/ron));
}
Complex reference(long double frequency,int mode,long double ron)
{
    const Complex s(0,2*std::numbers::pi_v<long double>*frequency);
    Matrix<Complex,9> a {};std::array<Complex,9>b {};
    resistors(a,mode,ron);
    edge(a,0,-1,s*10e-6L);b[0]=s*10e-6L;
    edge(a,1,2,s*4.7e-9L);edge(a,3,4,s*15e-9L);
    edge(a,6,7,s*47e-9L);edge(a,7,-1,s*10e-9L);
    // IC4b output as a separate unknown; ideal non-inverting input draws
    // no current through R20. C6 remains an independent feedback store.
    a[8][8]=s*22e-9L+1.L/100e3L;
    a[8][7]=-a[8][8]-1.L/10e3L;
    const auto v=solve(a,b);
    return v[2]+v[4]+v[5]+v[6]+(47.L/220)*v[8];
}

#ifndef YOUKNOW_HPF_BASELINE
using State = std::array<long double,6>;
struct OdeValue { State derivative {}; long double output {}; };
OdeValue nodeDerivative(const State& state,long double input,int mode,long double ron)
{
    // Capacitor branch currents are unknowns 8..12. Voltage constraints
    // prescribe all five physical charges; KCL determines instantaneous
    // currents. This is independent of the production discrete-time stamp.
    Matrix<long double,13>a {};std::array<long double,13>b {};
    resistors(a,mode,ron);
    constexpr int p[5]={-2,1,3,6,7},q[5]={0,2,4,7,-1};
    constexpr long double c[5]={10e-6L,4.7e-9L,15e-9L,47e-9L,10e-9L};
    for(int i=0;i<5;++i)
    {
        if(p[i]>=0){a[p[i]][8+i]+=1;a[8+i][p[i]]+=1;}
        if(q[i]>=0){a[q[i]][8+i]-=1;a[8+i][q[i]]-=1;}
        b[8+i]=state[i]-(p[i]==-2?input:0);
    }
    const auto v=solve(a,b);OdeValue out;
    for(int i=0;i<5;++i)out.derivative[i]=v[8+i]/c[i];
    out.derivative[5]=(v[7]/10000-state[5]/100000)/22e-9L;
    out.output=v[2]+v[4]+v[5]+v[6]+(47.L/220)*(v[7]+state[5]);
    return out;
}
struct Ode
{
    std::array<State,6> a {};State b {}, c {};long double d {};
    Ode(int mode,long double ron)
    {
        State zero {};auto drive=nodeDerivative(zero,1,mode,ron);b=drive.derivative;d=drive.output;
        for(int i=0;i<6;++i)
        {
            State basis {};basis[i]=1;auto v=nodeDerivative(basis,0,mode,ron);c[i]=v.output;
            for(int row=0;row<6;++row)a[row][i]=v.derivative[row];
        }
    }
    OdeValue eval(const State& v,long double input)const
    {
        OdeValue out;out.output=d*input;
        for(int i=0;i<6;++i)
        {out.derivative[i]=b[i]*input;out.output+=c[i]*v[i];
         for(int j=0;j<6;++j)out.derivative[i]+=a[i][j]*v[j];}
        return out;
    }
};
State offset(State x,const State& dx,long double h)
{for(int i=0;i<6;++i)x[i]+=h*dx[i];return x;}
long double integrate(const Ode& ode,State& state,long double input,long double h,int steps)
{
    long double integral=0;const long double dt=h/steps;
    for(int i=0;i<steps;++i)
    {
        const auto a=ode.eval(state,input),b=ode.eval(offset(state,a.derivative,dt/2),input);
        const auto c=ode.eval(offset(state,b.derivative,dt/2),input),d=ode.eval(offset(state,c.derivative,dt),input);
        for(int j=0;j<6;++j)state[j]+=dt/6*(a.derivative[j]+2*b.derivative[j]+2*c.derivative[j]+d.derivative[j]);
        integral+=dt/6*(a.output+2*b.output+2*c.output+d.output);
    }
    return integral/h;
}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void selfTest()
{
    double maxTransfer=0,maxState=0,maxSwitchOutput=0;
    for(double rate:{8000.,44100.,48000.,192000.,768000.})
    {
        double rateState=0,rateOutput=0;
        for(double ron:{50.,110.,240.,1000.})for(int mode=0;mode<4;++mode)
        {
            HighPassSwitchCircuit circuit;circuit.prepare(rate,ron);
            const auto identity=[](double x){return x;};
            using Probe=HighPassSwitchTestAccess;
            Matrix<Complex,6>a {};std::array<Complex,6>b {},c {};
            Probe::seed(circuit,{},mode);const double d=circuit.process(1,mode,identity);
            const auto drive=Probe::state(circuit);
            for(int i=0;i<6;++i)b[i]=drive[i];
            for(int col=0;col<6;++col)
            {
                Probe::State basis {};basis[col]=1;Probe::seed(circuit,basis,mode);
                c[col]=circuit.process(0,mode,identity);const auto state=Probe::state(circuit);
                for(int row=0;row<6;++row)a[row][col]=state[row];
            }
            for(double frequency:{.5,5.,30.,100.,500.,2000.,10000.,20000.})
            {
                if(frequency>=rate*.45)continue;
                const auto z=std::polar(1.L,2*std::numbers::pi_v<long double>*frequency/rate);
                auto matrix=a;
                for(int i=0;i<6;++i)for(int j=0;j<6;++j)matrix[i][j]=(i==j?z:Complex{})-a[i][j];
                const auto state=solve(matrix,b);Complex actual=d;
                for(int i=0;i<6;++i)actual+=c[i]*state[i];
                const long double warped=rate/std::numbers::pi_v<long double>*std::tan(std::numbers::pi_v<long double>*frequency/rate);
                const auto expected=reference(warped,mode,ron);
                maxTransfer=std::max(maxTransfer,static_cast<double>(std::abs(actual-expected)));
            }
            Probe::State initial {.02,.12,-.11,.15,.08,.25};
            Probe::seed(circuit,initial,(mode+1)%4);
            State state;std::copy(initial.begin(),initial.end(),state.begin());
            const Ode ode(mode,ron);const double h=1/rate;
            const int steps=std::max(64,static_cast<int>(std::ceil(h/(ron*8e-9*.025))));
            // Include thirty ordinary intervals after the refined window;
            // testing only its first sample hid a later fast-mode residue.
            const int samples=30+static_cast<int>(std::ceil(Probe::windowSeconds(circuit)*rate));
            for(int sample=0;sample<samples;++sample)
            {
                const long double expected=integrate(ode,state,.2L,h,steps);
                const double actual=circuit.process(.2,mode,identity);
                const auto got=Probe::state(circuit);
                for(int i=0;i<6;++i)rateState=std::max(rateState,static_cast<double>(std::abs(state[i]-got[i])));
                rateOutput=std::max(rateOutput,static_cast<double>(std::abs(expected-actual)));
            }
            const auto saved=Probe::state(circuit);circuit.prepare(rate*2,ron);
            require(Probe::state(circuit)==saved,"rate change lost capacitor charge");
        }
        // The former 100uV bound tested only the refined first interval.
        // Ordinary trapezoidal integration has O(h^2) global step error;
        // keep 100uV at48kHz+ and scale that fixture allowance below48kHz.
        // This qualifies the low-rate approximation, not an analogue voltage
        // tolerance. Report its actual error instead of hiding it with a
        // long, expensive fine-step tail through the slower C11/C10 poles.
        const double rateRatio=std::max(1.0,48000.0/rate);
        const double limit=1e-4*rateRatio*rateRatio;
        std::cout<<"rate "<<rate<<" full-trajectory capacitor error "<<rateState
                 <<", mean output error "<<rateOutput<<", bound "<<limit<<'\n';
        require(rateState<limit,"switch charge failed rate-qualified fine RK4 comparison");
        require(rateOutput<limit,"switch output failed rate-qualified fine RK4 comparison");
        maxState=std::max(maxState,rateState);maxSwitchOutput=std::max(maxSwitchOutput,rateOutput);
    }
    {
        using Probe=HighPassSwitchTestAccess;
        HighPassSwitchCircuit circuit;circuit.prepare(768000,240);
        circuit.process(.2,0,[](double x){return x;});
        const auto remaining=Probe::remainingSeconds(circuit);
        const auto saved=Probe::state(circuit);
        const double expectedWindow=10.0*240.0*(47e-9*10e-9/(47e-9+10e-9));
        require(std::abs(remaining-(expectedWindow-1.0/768000))<1e-15,
                "switch window is not ten physical Ron*Cseries time constants");
        circuit.prepare(192000,240);
        require(Probe::remainingSeconds(circuit)==remaining&&Probe::state(circuit)==saved,
                "rate change altered remaining physical switch time or charge");
        circuit.process(.2,0,[](double x){return x;});
        require(std::abs(Probe::remainingSeconds(circuit)-(remaining-1.0/192000))<1e-15,
                "switch countdown did not follow the new interval duration");
        circuit.reset();
        require(Probe::remainingSeconds(circuit)==0,"reset retained an old switch window");
    }
    std::cout<<"max complex transfer error "<<maxTransfer<<", full switch trajectory capacitor error "<<maxState
             <<", mean switch output error "<<maxSwitchOutput<<'\n';
    require(maxTransfer<1e-7,"component-node transfer mismatch");
    auto engine=std::make_unique<YouKnowEngine>();
    require(!engine->configureHighPassSwitch(0)&&!engine->configureHighPassSwitch(NAN),"invalid switch resistance accepted");
    require(engine->configureHighPassSwitch(110),"valid circuit configuration rejected");
    engine->prepare(48000,128,4);
    require(!engine->configureHighPassSwitch(240),"live circuit replacement accepted");
    const auto a=render(0,128),b=render(110,128),split=render(110,17);
    require(measure(a).rms>1e-5&&measure(b).rms>1e-5,"callback comparison was silent");
    require(a.left!=b.left,"configured circuit did not reach the audio callback");
    require(b.left==split.left&&b.right==split.right,"configured callback is block dependent");
    std::cout<<"HPF switch circuit self-test passed\n";
}
#endif

StereoBuffer render(double ron,int block)
{
    EngineParameters p;p.calibration=1;p.sawEnabled=true;p.pulseEnabled=true;
    p.subLevel=.4f;p.noiseLevel=0;p.chorus=ChorusMode::Off;p.chorusNoise=0;
    p.cutoff=.85f;p.resonance=.1f;p.envDepth=0;p.vcfLfoDepth=0;p.dcoLfoDepth=0;
    p.attack=0;p.decay=.2f;p.sustain=.7f;p.release=.3f;p.vcaLevel=.6f;p.volume=1;
    p.vcfTanhMode=VcfTanhMode::PolyZoned;p.vcfFastEarlyMode=VcfFastEarlyMode::Cubic;p.vcfSolverMode=VcfSolverMode::Rk4Single;
    auto engine=std::make_unique<YouKnowEngine>();
#ifndef YOUKNOW_HPF_BASELINE
    if(ron>0&&!engine->configureHighPassSwitch(ron))throw std::runtime_error("switch setup failed");
#else
    (void)ron;
#endif
    engine->prepare(48000,block,4);engine->setParameters(p);StereoBuffer audio;
    const auto rest=[&](double seconds){int left=std::lround(seconds*48000);while(left>0){int n=std::min(left,block);auto at=audio.left.size();audio.left.resize(at+n);audio.right.resize(at+n);engine->process(audio.left.data()+at,audio.right.data()+at,n);left-=n;}};
    rest(.1);for(int note:{36,48,55,60,64,67})engine->noteOn(note,1);
    for(auto mode:{HighPassMode::Boost,HighPassMode::One,HighPassMode::Two,HighPassMode::Three,HighPassMode::Boost,HighPassMode::Three,HighPassMode::Two,HighPassMode::One})
    {p.highPass=mode;engine->setParameters(p);rest(.3);}
    for(int note:{36,48,55,60,64,67})engine->noteOff(note);rest(.6);return audio;
}
void audition(const std::filesystem::path& directory)
{
    std::filesystem::create_directories(directory);std::string error;
    const std::array<StereoBuffer,3> audio {render(0),render(110),render(240)};
    const auto blocked=render(110,17);
    if(audio[1].left!=blocked.left||audio[1].right!=blocked.right)throw std::runtime_error("candidate is block dependent");
    std::array<Level,3> levels;
    double peak=0;
    for(int i=0;i<3;++i){levels[i]=measure(audio[i]);peak=std::max(peak,levels[i].peak);}
    const double common=.4/peak;
    std::ofstream key(directory/"key.md"),metrics(directory/"metrics.csv");
    key<<"A: nominal engine, comparison disabled. B/C: coupled C14/IC3/HPF with110/240ohm Ron.\n"
          "Toshiba10V/5V typical table points; neither is claimed to be this Juno's installed resistance.\n"
          "Same shipping Poly/Cubic/RK4 kernels,48kHz/4x,block128,seed,MIDI and controls.\n"
          "Whole-file stereo RMS matched. Raw WAVs retain gain. Keys are separate from lettered listening files.\n";
    metrics<<"letter,rms_dbfs,peak_dbfs,matching_gain_db,difference_rms_dbfs\n";
    for(int i=0;i<3;++i)
    {
        const std::string letter(1,static_cast<char>('A'+i));
        const double gain=common*levels[0].rms/levels[i].rms;StereoBuffer delta;
        if(!difference(audio[0],audio[i],delta,error)
           ||!writeFloatWav(directory/(letter+".wav"),applyGain(audio[i],gain),error)
           ||!writeFloatWav(directory/(letter+"-raw.wav"),audio[i],error))throw std::runtime_error(error);
        key<<letter<<" trim "<<decibels(gain)<<"dB\n";
        metrics<<letter<<','<<decibels(levels[i].rms)<<','<<decibels(levels[i].peak)<<','<<decibels(gain)<<','<<decibels(measure(delta).rms)<<'\n';
        if(i>0&&!writeFloatWav(directory/(letter+"-difference.wav"),delta,error))throw std::runtime_error(error);
    }
    if(!key||!metrics)throw std::runtime_error("could not write metadata");
    std::cout<<"HPF A/B/C rendered; candidate block128/17 bit-identical\n";
}
} // namespace

int main(int argc,char**argv)
{
    try
    {
#ifndef YOUKNOW_HPF_BASELINE
        if(argc==2&&std::string(argv[1])=="--self-test"){selfTest();return 0;}
#endif
        if(argc==3&&std::string(argv[1])=="--render"){audition(argv[2]);return 0;}
        throw std::runtime_error("usage: --self-test | --render DIRECTORY");
    }
    catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
