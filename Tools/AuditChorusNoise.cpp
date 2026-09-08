// Clock-grid reconstruction of the existing independent BBD noise source.
// This audit does not infer an MN3009 noise amplitude or PSD from its maximum
// A-weighted datasheet row. Given the existing iid edge source (variance v),
// the continuous held source has two-sided PSD v/fcp*sinc(f/fcp)^2. Sample it
// only after reconstruction; folding the raw staircase on a coarse host grid
// creates another, non-physical noise spectrum.
//
// --self-test independently integrates the continuous random staircase
// against the four-point Lagrange reconstruction kernel, with exact Gaussian
// quadrature on every clock/kernel interval. It also checks physical RNG/hold
// preservation. --measure DIR exports same-seed physical output at several
// rates/clocks for the Python high-rate/PSD audit. --render WAV FACTOR creates
// a short noise-focused audition through the shipping engine (FACTOR 1 or 4).
// Compile this exact source against e405d7a for the frozen A baseline.
#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"
#include <bit>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace youknow
{
struct YouKnowTestAccess
{
    static float core(Chorus& chorus, float clock, float rate)
    { return chorus.lineA_.processClockedCore(0.0f, clock, rate, 1.0f); }
    static auto noiseState(const Chorus& chorus) { return chorus.lineA_.noiseState; }
    static float held(const Chorus& chorus) { return chorus.lineA_.held; }
    static void seed(Chorus& chorus, std::uint32_t value) { chorus.lineA_.reset(value); }
    static float output(Chorus& chorus, float clock, float rate)
    { return chorus.lineA_.process(0.0f, clock, rate, chorus.support_.exactOutputConnected, 1.0f); }
};
}

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::uint32_t seed = 0x1234567u;
std::uint32_t next(std::uint32_t value)
{
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return value;
}
float sourceValue(std::uint32_t state)
{
    return (static_cast<float>(state & 0xffffffu)*(2.0f/16777215.0f)-1.0f)
        * Chorus::independentLineRandomAmplitude;
}
double kernel(double coordinate)
{
    const double x = std::abs(coordinate);
    if (x < 1.0) return (x+1)*(x-1)*(x-2)/2;
    if (x < 2.0) return -(x-1)*(x-2)*(x-3)/6;
    return 0;
}
double oracle(double time, double period, const std::vector<float>& values)
{
    // Split at every staircase edge AND every polynomial boundary. Three
    // point Gauss-Legendre quadrature exactly integrates each cubic segment;
    // neither a production BLEP residual nor its past/future sign is reused.
    std::vector<double> knots {time-2,time-1,time,time+1,time+2};
    const auto first = static_cast<long long>(std::floor((time-2)/period));
    const auto last = static_cast<long long>(std::ceil((time+2)/period));
    for (auto edge=first; edge<=last; ++edge)
        if (edge*period > time-2 && edge*period < time+2)
            knots.push_back(edge*period);
    std::sort(knots.begin(),knots.end());
    double result=0;
    constexpr double abscissa=0.774596669241483377;
    for (std::size_t i=1;i<knots.size();++i)
    {
        const double middle=(knots[i-1]+knots[i])/2;
        const double half=(knots[i]-knots[i-1])/2;
        const auto index=static_cast<long long>(std::floor(middle/period));
        const double value=index<0 ? 0 : values.at(static_cast<std::size_t>(index));
        result+=value*half*(5.0/9*kernel(time-middle+half*abscissa)
            +8.0/9*kernel(time-middle)+5.0/9*kernel(time-middle-half*abscissa));
    }
    return result;
}
void selfTest()
{
    double maximumError=0;
    for (const auto [rate,clock] : std::array<std::pair<float,float>,6>{{
        {48000,12000},{48000,37001},{44100,80000},{192000,37001},
        {8000,200000},{768000,200000}}})
    {
        Chorus chorus; chorus.prepare(rate); YouKnowTestAccess::seed(chorus,seed);
        const double period=double(rate)/clock;
        std::vector<float> values {0};
        std::vector<std::uint32_t> states {seed};
        const auto count=static_cast<std::size_t>(std::ceil(515/period))+2;
        for(std::size_t i=1;i<count;++i)
        { states.push_back(next(states.back())); values.push_back(sourceValue(states.back())); }
        for(int sample=1;sample<=512;++sample)
        {
            const double actual=YouKnowTestAccess::core(chorus,clock,rate);
            const double expected=oracle(sample,period,values);
            maximumError=std::max(maximumError,std::abs(actual-expected));
            // At non-commensurate floating boundaries, edge count can be one
            // sample side of an exact rational boundary; neither draw may be
            // consumed by lookahead. The held value must match the live RNG.
            const auto live=YouKnowTestAccess::noiseState(chorus);
            if(YouKnowTestAccess::held(chorus)!=sourceValue(live) && live!=seed)
                throw std::runtime_error("lookahead changed the physical held sample");
        }
    }
    // Quadrature is exact on each cubic segment apart from double rounding.
    // Each float jump subtraction has error <=2*epsilon*sourceAmplitude and
    // each residual weight has magnitude <=1/2. Bound every past/future slot,
    // then allow four more epsilons for the final sum/casts. This conservative
    // roundoff bound is independent of the observed/golden output error.
    constexpr double tolerance = (2*Chorus::maximumBlepEvents+4)
        * std::numeric_limits<float>::epsilon()*Chorus::independentLineRandomAmplitude;
    if(maximumError>tolerance)
        throw std::runtime_error("noise reconstruction quadrature error "+std::to_string(maximumError));
    std::cout << std::setprecision(10) << "continuous-staircase quadrature max error "
              << maximumError << ", roundoff bound " << tolerance
              << "; held/RNG preservation passed\n";
}
void measure(const std::filesystem::path& directory)
{
    if(std::filesystem::exists(directory)) throw std::runtime_error("output directory already exists");
    std::filesystem::create_directories(directory);
    std::ofstream index(directory/"index.csv");
    index << "file,sample_rate,clock_hz,seconds,settle_seconds,source_amplitude,elapsed_seconds\n";
    index << std::setprecision(17);
    static_assert(std::endian::native==std::endian::little);
    for(const int rate : {44100,48000,96000,192000,768000})
        for(const int clock : {20000,37001,80000,100000})
        {
            Chorus chorus; chorus.prepare(rate); YouKnowTestAccess::seed(chorus,seed);
            const auto start=std::chrono::steady_clock::now();
            constexpr int seconds=8;
            std::vector<float> output(static_cast<std::size_t>(rate)*seconds);
            for(auto& sample:output) sample=YouKnowTestAccess::output(chorus,float(clock),float(rate));
            const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
            const std::string name="noise-"+std::to_string(rate)+"-"+std::to_string(clock)+".f32";
            std::ofstream file(directory/name,std::ios::binary);
            file.write(reinterpret_cast<const char*>(output.data()),static_cast<std::streamsize>(output.size()*sizeof(float)));
            if(!file) throw std::runtime_error("cannot write noise measurement");
            index << name << ',' << rate << ',' << clock << ',' << seconds << ",0.5,"
                  << Chorus::independentLineRandomAmplitude << ',' << elapsed << '\n';
        }
    std::cout << "fixed-clock physical output measurements saved\n";
}
void exportSupport()
{
    constexpr float rate=768000;
    const auto transition=Chorus::supportChainFor(rate).exactOutputConnected;
    const auto rows=[](const auto& values) {
        std::cout << '[';
        bool first=true;
        for(const auto& row:values)
        {
            if(!first) std::cout << ',';
            first=false; std::cout << '[';
            bool firstValue=true;
            for(const auto value:row)
            { if(!firstValue) std::cout << ','; firstValue=false; std::cout << value; }
            std::cout << ']';
        }
        std::cout << ']';
    };
    std::cout << std::setprecision(17) << "{\"sample_rate\":" << rate << ",\"state_by_column\":";
    rows(transition.stateByColumn);
    std::cout << ",\"drive_by_sample\":"; rows(transition.driveBySample); std::cout << "}\n";
}
StereoBuffer render(int factor, int block)
{
    YouKnowEngine engine; engine.prepare(comparisonSampleRate,comparisonBlockSize,factor);
    EngineParameters p;
    p.vcfTanhMode=VcfTanhMode::PolyZoned; p.vcfFastEarlyMode=VcfFastEarlyMode::Cubic;
    p.vcfSolverMode=VcfSolverMode::Rk4Single;
    p.chorus=ChorusMode::One; p.sawEnabled=false; p.pulseEnabled=false;
    p.subLevel=0; p.noiseLevel=0; p.calibration=1; p.volume=1;
    engine.setParameters(p);
    StereoBuffer output; output.left.resize(6*comparisonSampleRate); output.right.resize(output.left.size());
    std::size_t cursor=0;
    while(cursor<output.left.size())
    {
        const auto count=std::min<std::size_t>(block,output.left.size()-cursor);
        engine.process(output.left.data()+cursor,output.right.data()+cursor,static_cast<int>(count));
        cursor+=count;
    }
    return output;
}
}
int main(int argc,char** argv)
{
    try
    {
        if(argc==2 && std::string(argv[1])=="--self-test") selfTest();
        else if(argc==2 && std::string(argv[1])=="--support") exportSupport();
        else if(argc==3 && std::string(argv[1])=="--measure") measure(argv[2]);
        else if(argc==4 && std::string(argv[1])=="--render")
        {
            const std::string factorText(argv[3]);
            if(factorText!="1" && factorText!="4") throw std::runtime_error("factor must be 1 or 4");
            const int factor=std::stoi(factorText);
            if(std::filesystem::exists(argv[2])) throw std::runtime_error("output WAV already exists");
            const auto output=render(factor,128);
            const auto split=render(factor,17);
            if(output.left!=split.left || output.right!=split.right)
                throw std::runtime_error("shipping engine noise depends on block partition");
            std::string error;
            if(!writeFloatWav(argv[2],output,error)) throw std::runtime_error(error);
            std::cout << "6s shipping noise; " << factor << "x, block128/17 bit-identical; raw RMS "
                      << decibels(measure(output).rms) << "dBFS\n";
        }
        else throw std::runtime_error("usage: --self-test | --support | --measure NEWDIR | --render WAV 1|4");
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
