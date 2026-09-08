// A/B of C58's fixed-RC approximation and the coupled transistor load.
// Same shipping kernels, 48kHz/4x, seed, score and panel; only that circuit
// changes. Raw archives are retained, listening files match whole-file RMS.
// Build with YOUKNOW_VCA_BASELINE against 7ed16c4 headers/library to verify A.
#include "DSP/YouKnowEngine.h"
#include "RealismComparisonSupport.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
StereoBuffer render(bool coupled)
{
    EngineParameters p;
    p.calibration=1; p.sawEnabled=true; p.pulseEnabled=false;
    p.subLevel=0; p.noiseLevel=0; p.chorus=ChorusMode::Off;
    p.cutoff=.8f; p.resonance=.1f; p.envDepth=0; p.vcfLfoDepth=0;
    p.dcoLfoDepth=0; p.highPass=HighPassMode::One;
    p.attack=0; p.decay=.2f; p.sustain=.025f; p.release=.12f;
    p.volume=1; p.vcaLevel=.5f; p.polyphony=6;
    p.vcfTanhMode=VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode=VcfFastEarlyMode::Cubic;
    p.vcfSolverMode=VcfSolverMode::Rk4Single;
#ifndef YOUKNOW_VCA_BASELINE
    p.enableCoupledVoiceVcaControl=coupled;
#else
    (void)coupled;
#endif
    auto engine=std::make_unique<YouKnowEngine>();
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(48000,128,4); engine->setParameters(p);
    StereoBuffer audio;
    const auto rest=[&](double seconds) {
        int left=static_cast<int>(std::lround(seconds*48000));
        while(left>0)
        {
            const int frames=std::min(128,left);
            const auto at=audio.left.size();
            audio.left.resize(at+frames); audio.right.resize(at+frames);
            engine->process(audio.left.data()+at,audio.right.data()+at,frames);
            left-=frames;
        }
    };
    rest(.1);
    for(int note:{48,55,60,64,48,55,60,64})
    {
        engine->noteOn(note,1);rest(.12);engine->noteOff(note);rest(.25);
    }
    p.vcaMode=VcaMode::Gate;engine->setParameters(p);
    for(int repetition=0;repetition<6;++repetition)
    {
        for(int note:{48,55,60,64,67,72})engine->noteOn(note,1);
        rest(.08);
        for(int note:{48,55,60,64,67,72})engine->noteOff(note);
        rest(.12);
    }
    rest(.4);return audio;
}
}

int main(int argc,char**argv)
{
    try
    {
        if(argc!=2)throw std::runtime_error("usage: YouKnowRenderVcaControlComparison <output-directory>");
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        const auto start=std::chrono::steady_clock::now();
        const auto a=render(false);
        const auto middle=std::chrono::steady_clock::now();
        const auto b=render(true);
        const auto finish=std::chrono::steady_clock::now();
        const auto levelA=measure(a),levelB=measure(b);
        const double common=.25/std::max(levelA.peak,levelB.peak);
        const double matching=levelA.rms/levelB.rms;
        std::string error;StereoBuffer delta;
        if(!difference(a,b,delta,error))throw std::runtime_error(error);
        for(const auto& item: {std::pair{"A-raw.wav",a},std::pair{"B-raw.wav",b},
              std::pair{"A.wav",applyGain(a,common)},
              std::pair{"B.wav",applyGain(b,common*matching)},std::pair{"difference.wav",delta}})
            if(!writeFloatWav(directory/item.first,item.second,error))throw std::runtime_error(error);
        std::ofstream key(directory/"key.md");
        key<<"A: previous fixed 687us C58 pole. B: coupled C58/Tr20 load.\n"
             "Identical shipping Poly/Cubic/RK4, 48kHz/4x, Unit Character1, seed and score.\n"
             "Listening files match whole-file stereo RMS; raw files preserve measured levels.\n"
           <<"A gain "<<decibels(common)<<"dB; B gain "<<decibels(common*matching)<<"dB.\n"
           <<"Raw RMS change "<<decibels(levelB.rms/levelA.rms)<<"dB; difference RMS "
           <<decibels(measure(delta).rms)<<"dBFS.\n";
        std::ofstream metrics(directory/"metrics.csv");
        metrics<<"raw_a_rms_dbfs,raw_b_rms_dbfs,difference_rms_dbfs,a_render_seconds,b_render_seconds\n"
          <<decibels(levelA.rms)<<','<<decibels(levelB.rms)<<','<<decibels(measure(delta).rms)<<','
          <<std::chrono::duration<double>(middle-start).count()<<','
          <<std::chrono::duration<double>(finish-middle).count()<<'\n';
        if(!key||!metrics)throw std::runtime_error("could not write comparison metadata");
        std::cout<<"RMS change "<<decibels(levelB.rms/levelA.rms)<<"dB; difference "
                 <<decibels(measure(delta).rms)<<"dBFS\n";
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
