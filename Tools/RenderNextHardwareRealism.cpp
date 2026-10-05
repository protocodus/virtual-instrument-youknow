// Further five-mechanism comparison against frozen shipping629087f.
// Compile this identical source with frozen or current DSP headers/library.
// Three tone passages explicitly use Direct; panel/full-tone passages use
// Original in both builds. All shipping circuit selections stay active.
// Macro YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE selects only the new inbound
// full-tone API versus the old processor's snapshot route. It never changes
// the score, physical controls, note timing or serialization bytes.
// 48k stereo/1x, six voices,128-frame event-split callbacks, settled existing
// software thermal prior, fixed seeds,250ms preroll, declared latency only.
// Audible differences are changes, not measurements of hardware accuracy.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductHardwareRealism.h"
#include "DSP/YouKnowSysEx.h"
#include "RealismComparisonSupport.h"

#ifndef YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE
#define YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE 0
#endif
#ifndef YOUKNOW_DSP_SOURCE_SHA256
#define YOUKNOW_DSP_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_NEXT_HARDWARE_REALISM_SOURCE_SHA256
#define YOUKNOW_NEXT_HARDWARE_REALISM_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_ID
#define YOUKNOW_COMPARISON_COMPILER_ID "unknown"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_VERSION
#define YOUKNOW_COMPARISON_COMPILER_VERSION "unknown"
#endif

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::string_view protocol = "youknow-next-hardware-realism-v1";
constexpr std::array<std::string_view, 5> names {
    "01-chorus-insertion", "02-quiet-temperature", "03-resonance-offset",
    "04-panel-routing", "05-full-tone-routing" };

void require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

std::string safeWord(std::string_view value)
{
    require(!value.empty() && value != "." && value != ".."
        && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
            == std::string_view::npos, "labels/revisions must contain letters, numbers, -_. only");
    return std::string(value);
}

std::string quoted(std::string_view value)
{
    std::string result = "\"";
    for (const char c : value)
    {
        if (c == '\\' || c == '"') result += '\\';
        require(static_cast<unsigned char>(c) >= 32, "unexpected control character");
        result += c;
    }
    return result + '"';
}

std::string patchJson(const EngineParameters& p)
{
    // The physical controls are identical in the two builds. Candidate-only
    // circuit choices belong to the build/profile, never hidden score changes.
    std::ostringstream out;
    out << std::setprecision(9) << "{\"saw\":" << p.sawEnabled
        << ",\"pulse\":" << p.pulseEnabled << ",\"sub\":" << p.subLevel
        << ",\"noise\":" << p.noiseLevel << ",\"pwm_source\":" << int(p.pwmSource)
        << ",\"pwm_depth\":" << p.pwmDepth << ",\"lfo_rate\":" << p.lfoRate
        << ",\"cutoff\":" << p.cutoff << ",\"resonance\":" << p.resonance
        << ",\"env_depth\":" << p.envDepth << ",\"key_follow\":" << p.keyFollow
        << ",\"attack\":" << p.attack << ",\"decay\":" << p.decay
        << ",\"sustain\":" << p.sustain << ",\"release\":" << p.release
        << ",\"vca_mode\":" << int(p.vcaMode) << ",\"vca_level\":" << p.vcaLevel
        << ",\"high_pass\":" << int(p.highPass) << ",\"chorus\":" << int(p.chorus)
        << ",\"chorus_noise\":" << p.chorusNoise << ",\"volume\":" << p.volume << '}';
    return out.str();
}

EngineParameters panel(bool candidate)
{
    EngineParameters p;
#if !YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE
    require(!candidate, "this executable has no new routing API");
#else
    (void) candidate;
#endif
    ProductHardwareRealismProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1; p.aging = .5f; p.velocityDepth = 0; p.polyphony = 6;
    p.outputRoute = HeadphoneOutput::Route::Line;
    p.outputSelector = OutputNetwork::Selector::High;
    p.outputLoadOhms = 0; p.outputCapacitancePf = 0; p.outputMono = false;
    p.volume = .6f; p.sawEnabled = true; p.pulseEnabled = false;
    p.pwmSource = PwmSource::Manual; p.pwmDepth = .35f; p.lfoRate = .42f;
    p.subLevel = 0; p.noiseLevel = 0; p.chorusNoise = 0;
    p.cutoff = .70f; p.resonance = .15f; p.envDepth = 0; p.keyFollow = 0;
    p.vcfLfoDepth = 0; p.dcoLfoDepth = 0; p.highPass = HighPassMode::One;
    p.vcaMode = VcaMode::Gate; p.vcaLevel = 1;
    p.attack = 0; p.decay = .2f; p.sustain = .7f; p.release = .1f;
    p.chorus = ChorusMode::Off;
    return p;
}

struct Scene
{
    StereoBuffer audio;
    int latency {};
    std::string initialPatch;
    std::string events;
    bool original {};
};

class Performance
{
public:
    Performance(const EngineParameters& p, bool candidate, bool original = false)
        : engine_(std::make_unique<YouKnowEngine>()), initialPatch_(patchJson(p)), original_(original)
    {
#if !YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE
        require(!candidate, "this executable has no new routing API");
#else
        (void) candidate;
#endif
        ProductHardwareRealismProfile::configureBeforePrepare(*engine_);
        require(engine_->configureThermalStart(true), "cannot configure settled thermal start");
        engine_->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine_->setParameters(p);
        engine_->setOriginalPerformanceMode(original);
        engine_->prepare(comparisonSampleRate, comparisonBlockSize, 1);
        latency_ = engine_->getProcessingLatencySamples();
        require(latency_ >= 0 && latency_ < 4096, "invalid declared processing latency");
        run(.25, false);
    }

    void run(double seconds, bool retain = true)
    {
        int remaining = static_cast<int>(std::llround(seconds * comparisonSampleRate));
        while (remaining > 0)
        {
            const int count = std::min(remaining, comparisonBlockSize);
            engine_->process(left_.data(), right_.data(), count);
            require(!original_ || engine_->originalPerformanceHealthy(), "original firmware pipeline failed");
            if (retain)
            {
                audio_.left.insert(audio_.left.end(), left_.begin(), left_.begin() + count);
                audio_.right.insert(audio_.right.end(), right_.begin(), right_.begin() + count);
            }
            remaining -= count;
        }
    }

    void on(int note)
    {
        event("note_on", "\"note\":" + std::to_string(note) + ",\"velocity\":1");
        engine_->noteOn(note, 1);
    }
    void off(int note)
    {
        event("note_off", "\"note\":" + std::to_string(note));
        engine_->noteOff(note);
    }
    void set(const EngineParameters& p)
    {
        event("panel", "\"patch\":" + patchJson(p));
        engine_->setParameters(p);
    }
    void fullTone(const EngineParameters& p)
    {
        sysex::Patch patch;
        patch.lfoRate=p.lfoRate; patch.lfoDelay=p.lfoDelay; patch.dcoLfo=p.dcoLfoDepth;
        patch.pwm=p.pwmDepth; patch.noise=p.noiseLevel; patch.cutoff=p.cutoff;
        patch.resonance=p.resonance; patch.vcfEnv=p.envDepth; patch.vcfLfo=p.vcfLfoDepth;
        patch.keyFollow=p.keyFollow; patch.vcaLevel=p.vcaLevel; patch.attack=p.attack;
        patch.decay=p.decay; patch.sustain=p.sustain; patch.release=p.release;
        patch.sub=p.subLevel; patch.range=p.range; patch.saw=p.sawEnabled;
        patch.pulse=p.pulseEnabled; patch.pwmSource=p.pwmSource; patch.vcaMode=p.vcaMode;
        patch.envPolarity=p.envPolarity; patch.highPass=p.highPass; patch.chorus=p.chorus;
        std::array<std::uint8_t, sysex::patchMessageBytes> message {};
        require(sysex::writePatchMessage(patch, 0, message.data(), message.size()) == message.size(),
                "full-tone message encoding failed");
        std::ostringstream raw;
        raw << "\"bytes\":[";
        for (std::size_t i=0;i<message.size();++i) raw << (i ? "," : "") << unsigned(message[i]);
        raw << "],\"patch\":" << patchJson(p);
        event("full_tone_midi", raw.str());
#if YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE
        require(engine_->receiveOriginalPerformanceMidi(message), "full-tone input queue failed");
        engine_->setParameters(p, YouKnowEngine::ParameterInputSource::MidiReflection);
#else
        // Frozen629087f processor parsed a full-tone frame to this snapshot,
        // losing message identity before its OriginalPerformance adapter.
        engine_->setParameters(p);
#endif
    }
    void chord(std::initializer_list<int> notes, double hold, double gap)
    {
        for (const int note : notes) on(note);
        run(hold);
        for (const int note : notes) off(note);
        run(gap);
    }
    Scene take()
    {
        const auto expected = audio_.left.size();
        // Pull the delayed end, then discard precisely the declared latency.
        // No cross-correlation or alignment hides real timing differences.
        run(double(latency_) / comparisonSampleRate);
        audio_.left.erase(audio_.left.begin(), audio_.left.begin() + latency_);
        audio_.right.erase(audio_.right.begin(), audio_.right.begin() + latency_);
        require(audio_.left.size() == expected, "latency compensation changed duration");
        return { std::move(audio_), latency_, initialPatch_, '[' + events_ + ']', original_ };
    }

private:
    void event(std::string_view kind, const std::string& body)
    {
        if (!events_.empty()) events_ += ',';
        events_ += "{\"frame\":" + std::to_string(audio_.left.size())
            + ",\"type\":" + quoted(kind) + ',' + body + '}';
    }
    std::unique_ptr<YouKnowEngine> engine_;
    std::array<float, comparisonBlockSize> left_ {}, right_ {};
    StereoBuffer audio_;
    int latency_ {};
    std::string initialPatch_, events_;
    bool original_ {};
};

Scene chorusInsertion(bool candidate)
{
    auto p = panel(candidate);
    p.pulseEnabled=true; p.pwmSource=PwmSource::Lfo; p.pwmDepth=.22f;
    p.subLevel=.25f; p.cutoff=.56f; p.resonance=.28f; p.chorus=ChorusMode::One;
    Performance take(p,candidate);
    for(int note:{48,55,60,64}) take.on(note);
    take.run(4);
    p.chorus=ChorusMode::Two; take.set(p); take.run(3.5);
    for(int note:{48,55,60,64}) take.off(note);
    take.run(.5);
    return take.take();
}

Scene quietTemperature(bool candidate)
{
    auto p=panel(candidate);
    p.vcaMode=VcaMode::Envelope; p.cutoff=.53f; p.resonance=.24f;
    p.attack=YouKnowEngine::panelPositionForAttack(.008f);
    p.decay=YouKnowEngine::panelPositionForDecay(.35f); p.sustain=.05f;
    p.release=YouKnowEngine::panelPositionForRelease(1.0f);
    Performance take(p,candidate);
    take.on(48); take.run(1.1); take.off(48); take.run(1.4);
    p.sustain=.02f; take.set(p);
    take.on(60); take.run(1.1); take.off(60); take.run(1.4);
    return take.take();
}

Scene resonanceOffset(bool candidate)
{
    auto p=panel(candidate);
    p.cutoff=.36f; p.subLevel=.25f; p.pulseEnabled=true; p.resonance=.55f;
    Performance take(p,candidate);
    take.on(43);
    for(float value:{.55f,.72f,.85f,.97f,1.f}) {
        p.resonance=value; take.set(p); take.run(.6);
    }
    take.off(43); take.run(1);
    return take.take();
}

Scene panelRouting(bool candidate)
{
    auto p=panel(candidate);
    p.cutoff=.42f; p.resonance=.72f; p.pulseEnabled=true; p.subLevel=.35f;
    Performance take(p,candidate,true);
    take.on(48); take.run(.4);
    // Physical gesture snapshots: the old adapter incorrectly serialized them
    // as incoming MIDI parameter messages; the candidate updates ADC inputs.
    for(int i=0;i<28;++i) {
        const float phase=float(i)/27;
        p.cutoff=.18f+.64f*phase;
        p.pwmDepth=.12f+.65f*phase;
        p.subLevel=.15f+.60f*(1-phase);
        take.set(p); take.run(.1);
    }
    take.off(48); take.run(.8);
    return take.take();
}

Scene fullToneRouting(bool candidate)
{
    auto p=panel(candidate);
    p.cutoff=.38f; p.resonance=.72f; p.subLevel=.3f; p.pulseEnabled=true;
    Performance take(p,candidate,true);
    take.on(48); take.run(.8);
    p.lfoRate=.64f; p.lfoDelay=.2f; p.dcoLfoDepth=.03f; p.pwmDepth=.78f;
    p.noiseLevel=.04f; p.cutoff=.75f; p.resonance=.23f; p.envDepth=.15f;
    p.vcfLfoDepth=.08f; p.keyFollow=.25f; p.vcaLevel=.9f;
    p.attack=.05f; p.decay=.4f; p.sustain=.3f; p.release=.4f;
    p.subLevel=.75f; p.range=DcoRange::Sixteen; p.sawEnabled=false;
    p.pwmSource=PwmSource::Lfo; p.vcaMode=VcaMode::Envelope;
    p.highPass=HighPassMode::Boost; p.chorus=ChorusMode::One;
    take.fullTone(p); take.on(55); take.run(1.2);
    // An identical received frame remains a real wire/firmware event.
    take.fullTone(p); take.on(60); take.run(.5);
    p.cutoff=.32f; p.resonance=.82f; p.sawEnabled=true; p.chorus=ChorusMode::Off;
    p.subLevel=.15f; p.range=DcoRange::Eight; p.vcaMode=VcaMode::Gate;
    take.fullTone(p); take.run(.7);
    take.off(48); take.off(55); take.off(60); take.run(.8);
    return take.take();
}

std::string audioHash(const StereoBuffer& audio)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t frame = 0; frame < audio.left.size(); ++frame)
        for (const float sample : { audio.left[frame], audio.right[frame] })
        {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (int byte = 0; byte < 4; ++byte)
            {
                hash ^= (bits >> (byte * 8)) & 0xffu;
                hash *= 1099511628211ull;
            }
        }
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

void writeAudio(const std::filesystem::path& path, const StereoBuffer& audio)
{
    require(!std::filesystem::exists(path), "refusing to overwrite frozen raw audio");
    std::string error;
    require(validate(audio, error) && writeFloatWav(path, audio, error), error);
}

void render(const std::filesystem::path& directory, const std::string& label,
            bool candidate, const std::string& revision)
{
    const auto raw = directory / "raw" / label;
    require(!std::filesystem::exists(raw), "label output already exists; choose a fresh label");
    const auto started = std::chrono::steady_clock::now();
    const std::array<Scene, 5> scenes { chorusInsertion(candidate), quietTemperature(candidate),
        resonanceOffset(candidate), panelRouting(candidate), fullToneRouting(candidate) };
    StereoBuffer montage;
    std::ostringstream manifest;
    manifest << std::setprecision(15)
        << "{\n\"protocol\":" << quoted(protocol)
        << ",\n\"label\":" << quoted(label) << ",\n\"git_revision\":" << quoted(revision)
        << ",\n\"variant\":" << quoted(candidate ? "candidate" : "baseline")
        << ",\n\"candidate_apis_available\":" << YOUKNOW_NEXT_HARDWARE_REALISM_AVAILABLE
        << ",\n\"dsp_source_sha256\":" << quoted(YOUKNOW_DSP_SOURCE_SHA256)
        << ",\n\"renderer_source_sha256\":" << quoted(YOUKNOW_NEXT_HARDWARE_REALISM_SOURCE_SHA256)
        << ",\n\"compiler\":" << quoted(YOUKNOW_COMPARISON_COMPILER_ID " " YOUKNOW_COMPARISON_COMPILER_VERSION)
        << ",\n\"sample_rate\":48000,\"block_size\":128,\"quality_requested\":1,\"polyphony\":6"
        << ",\n\"output_route\":\"LINE\",\"output_selector\":\"High\",\"output_load_ohms\":0,\"output_capacitance_pf\":0"
        << ",\n\"latency_compensation\":\"declared integer processing latency only\""
        << ",\n\"discarded_preroll_seconds\":0.25,\"thermal_start\":\"settled\""
        << ",\n\"noise_seeds\":\"fixed prepare/reset seeds\",\"product_profile\":true"
        << ",\n\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",\"unit_character\":1,\"aging\":0.5"
        << ",\n\"score_kind\":\"all current product selections; Direct tone passages, Original inbound-routing passages\""
        << ",\n\"firmware_event_sources\":\"panel gestures and full-tone MIDI retain distinct declared input origins\""
        << ",\n\"performance_timing\":\"Direct for sections1-3; Original for sections4-5 in both builds\""
        << ",\n\"sections\":[\n";
    for (std::size_t index = 0; index < scenes.size(); ++index)
    {
        const auto& scene = scenes[index];
        const auto level = measure(scene.audio);
        const auto offset = montage.left.size();
        writeAudio(raw / (std::string(names[index]) + ".wav"), scene.audio);
        montage.left.insert(montage.left.end(), scene.audio.left.begin(), scene.audio.left.end());
        montage.right.insert(montage.right.end(), scene.audio.right.begin(), scene.audio.right.end());
        manifest << "{\"slug\":" << quoted(names[index]) << ",\"start_frame\":" << offset
            << ",\"frames\":" << scene.audio.left.size() << ",\"latency_samples\":" << scene.latency
            << ",\"peak_dbfs\":" << decibels(level.peak) << ",\"rms_dbfs\":" << decibels(level.rms)
            << ",\"performance_timing\":" << quoted(scene.original ? "Original" : "Direct")
            << ",\"float_fnv1a64\":" << quoted(audioHash(scene.audio))
            << ",\"initial_patch\":" << scene.initialPatch << ",\"events\":" << scene.events
            << '}' << (index + 1 < scenes.size() ? ",\n" : "\n");
        std::cout << names[index] << ": " << scene.audio.left.size() << " frames\n";
    }
    require(montage.left.size() == comparisonSampleRate * 25u, "score duration must be25s");
    writeAudio(raw / "montage.wav", montage);
    const auto level = measure(montage);
    manifest << "],\n\"montage\":{\"frames\":" << montage.left.size()
        << ",\"peak_dbfs\":" << decibels(level.peak) << ",\"rms_dbfs\":" << decibels(level.rms)
        << ",\"float_fnv1a64\":" << quoted(audioHash(montage)) << "},\n\"render_seconds\":"
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << "\n}\n";
    std::ofstream out(raw / "manifest.json");
    out << manifest.str(); out.close();
    require(bool(out), "cannot write render manifest");
    std::cout << "raw archive: " << raw.string() << '\n';
}
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 6 && std::string_view(argv[1]) == "render",
            "usage: YouKnowRenderNextHardwareRealism render DIR LABEL baseline|candidate REVISION");
        const auto label = safeWord(argv[3]);
        const std::string_view variant(argv[4]);
        require(variant == "baseline" || variant == "candidate", "variant must be baseline or candidate");
        render(argv[2], label, variant == "candidate", safeWord(argv[5]));
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n'; return 1;
    }
}
