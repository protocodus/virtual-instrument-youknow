// Reproducible build-to-build listening protocol, 2026-10-15, revision 1.
// Compile this SAME source against each revision's OWN headers and DSP archive.
// The active product profile, Original performance timing and 1x quality match
// new plug-in instances. Direct timing and 2x/4x are explicit diagnostic options.
// Every take uses 48 kHz, at most 128 frames per callback (split exactly at
// events), the engine's fixed reset/card/noise seeds, settled thermal state,
// 250 ms discarded preroll and an integer-frame score. We never reset between
// score sections, gate noise, remove silence or alter gain inside the renderer.
//
// --render SCORE OUTPUT.wav [original|direct] [1|2|4] writes raw float32 audio
// and emits its complete event/patch manifest as JSON. --list lists the scores.
// MakeFidelityQuickWins.py preserves the raw pair and produces whole-file
// stereo-RMS-matched, letter-only PCM24 listening WAVs with one shared peak
// trim. A means unchanged shipping; B means the candidate. A build-to-build
// comparison is NOT mechanism-isolated unless the build changes only that
// mechanism; the packaging tool records the declared comparison scope.
// No hardware capture or listening verdict is implied by these fixtures.
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr int rate = 48000;
constexpr int blockSize = 128;
constexpr int prerollFrames = rate / 4;
constexpr std::array<const char*, 10> names {
    "oscillator", "pulse", "sub", "unison", "chorus", "switching", "pedal", "ensemble",
    "local-transpose", "velocity-zero"
};

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Event
{
    int frame {};
    enum Kind { NoteOn, NoteOff, Pedal, Patch, LocalOn, LocalOff } kind {};
    int note {};
    float value {};
    EngineParameters patch;
};

struct Score
{
    EngineParameters initial;
    std::vector<Event> events;
    int frames = 8 * rate;
    std::string description;

    void note(double start, double duration, int pitch, float velocity = 1.0f)
    {
        events.push_back({static_cast<int>(std::llround(start * rate)), Event::NoteOn, pitch, velocity, {}});
        events.push_back({static_cast<int>(std::llround((start + duration) * rate)), Event::NoteOff, pitch, 0.0f, {}});
    }
    void pedal(double time, bool down)
    {
        events.push_back({static_cast<int>(std::llround(time * rate)), Event::Pedal, 0, down ? 1.0f : 0.0f, {}});
    }
    void patch(double time, const EngineParameters& p)
    {
        events.push_back({static_cast<int>(std::llround(time * rate)), Event::Patch, 0, 0.0f, p});
    }
    void local(double time, int pitch, bool on)
    {
        events.push_back({static_cast<int>(std::llround(time * rate)),
                          on ? Event::LocalOn : Event::LocalOff, pitch, on ? 1.0f : 0.0f, {}});
    }
    void zeroOff(double time, int pitch)
    {
        events.push_back({static_cast<int>(std::llround(time * rate)), Event::NoteOn, pitch, 0.0f, {}});
    }
};

Score scoreFor(const std::string& name)
{
    Score s;
    auto& p = s.initial;
    ActiveProductFidelityProfile::applyTo(p);
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1.0f; p.aging = 0.0f; p.polyphony = 6;
    p.sawEnabled = true; p.pulseEnabled = false;
    p.range = DcoRange::Eight; p.subLevel = 0.0f; p.noiseLevel = 0.0f;
    p.pwmDepth = 0.0f; p.pwmSource = PwmSource::Manual;
    p.cutoff = 1.0f; p.resonance = 0.0f; p.envDepth = 0.0f; p.keyFollow = 0.0f;
    p.dcoLfoDepth = p.vcfLfoDepth = 0.0f;
    p.attack = 0.0f; p.decay = 0.15f; p.sustain = 0.75f; p.release = 0.08f;
    p.vcaLevel = 0.7f; p.volume = 0.55f;
    p.chorus = ChorusMode::Off; p.highPass = HighPassMode::One;
    p.velocityDepth = 0.0f;

    if (name == "oscillator")
    {
        s.description = "Exposed saw: low, middle and high notes, repeated high attacks and a wide six-note chord. Chorus off.";
        s.note(0.1, 0.8, 36); s.note(1.1, 0.8, 60); s.note(2.1, 0.8, 84);
        for (int i = 0; i < 4; ++i) s.note(3.2 + i * 0.35, 0.22, 96);
        for (int n : {36, 48, 55, 67, 76, 84}) s.note(4.8, 2.0, n);
    }
    else if (name == "pulse")
    {
        s.description = "Pulse only: high notes at manual narrow width, then slow LFO PWM across a six-note spread. Chorus off.";
        p.sawEnabled = false; p.pulseEnabled = true; p.pwmDepth = 0.9f;
        s.note(0.1, 0.8, 60); s.note(1.1, 0.8, 84); s.note(2.1, 0.8, 96);
        auto q = p; q.pwmSource = PwmSource::Lfo; q.pwmDepth = 0.9f; q.lfoRate = 0.6f;
        s.patch(3.1, q);
        for (int n : {48, 55, 60, 72, 79, 84}) s.note(3.3, 3.4, n);
    }
    else if (name == "sub")
    {
        s.description = "Isolated divider/sub: high notes at 4-foot range expose edge timing, followed by a bass phrase. Chorus off.";
        p.sawEnabled = p.pulseEnabled = false; p.subLevel = 1.0f;
        p.range = DcoRange::Four; p.calibration = 0.0f;
        s.note(0.1, 1.0, 84); s.note(1.3, 1.0, 96); s.note(2.5, 1.0, 108);
        auto q = p; q.range = DcoRange::Eight; s.patch(3.7, q);
        const std::array<int, 6> notes {36, 43, 48, 46, 43, 36};
        for (std::size_t i = 0; i < notes.size(); ++i) s.note(3.9 + i * 0.45, 0.3, notes[i]);
    }
    else if (name == "unison")
    {
        s.description = "Six-card Unison: repeated bass attacks, held-note legato returns and portamento. No chorus or artificial detune.";
        p.keyMode = KeyMode::Unison; p.cutoff = 0.75f; p.resonance = 0.25f;
        p.pulseEnabled = true; p.pwmDepth = 0.45f; p.subLevel = 0.3f;
        for (int i = 0; i < 4; ++i) s.note(0.1 + i * 0.45, 0.28, 43);
        s.note(2.1, 2.0, 48); s.note(2.65, 0.9, 55); s.note(3.0, 0.3, 60);
        auto q = p; q.portamento = 0.3f; s.patch(4.3, q);
        s.note(4.5, 2.2, 43); s.note(5.2, 0.9, 67);
    }
    else if (name == "chorus")
    {
        s.description = "Chorus I then II: exposed high partials followed by a held wide pad; tails retain the natural noise floor.";
        p.chorus = ChorusMode::One; p.pulseEnabled = true; p.pwmDepth = 0.65f;
        s.note(0.1, 1.2, 84); s.note(1.5, 1.2, 96);
        auto q = p; q.cutoff = 0.7f; q.attack = 0.15f; q.release = 0.22f;
        s.patch(2.9, q);
        for (int n : {43, 50, 57, 62, 69, 76}) s.note(3.1, 3.3, n);
        q.chorus = ChorusMode::Two; s.patch(4.7, q);
    }
    else if (name == "switching")
    {
        s.description = "A held bright chord through Off, I, II, I+II and Off; listen to switching, modulation and the final tail.";
        p.pulseEnabled = true; p.pwmDepth = 0.4f; p.cutoff = 0.85f;
        for (int n : {48, 55, 62, 69, 76, 83}) s.note(0.1, 6.2, n);
        auto q = p;
        q.chorus = ChorusMode::One; s.patch(1.1, q);
        q.chorus = ChorusMode::Two; s.patch(2.6, q);
        q.chorus = ChorusMode::OneTwo; s.patch(4.1, q);
        q.chorus = ChorusMode::Off; s.patch(5.6, q);
    }
    else if (name == "pedal")
    {
        s.description = "Six-voice Unison with rapid pedal reversals; each note must release after queued down/up edges.";
        p.keyMode = KeyMode::Unison;
        p.cutoff = 0.62f; p.envDepth = 0.25f; p.decay = 0.25f; p.sustain = 0.6f;
        p.release = 0.1f;
        s.note(0.1, 0.6, 48);
        s.pedal(0.3, true); s.pedal(0.3 + 1.0 / rate, false);
        s.note(2.2, 0.6, 50);
        s.pedal(2.4, true); s.pedal(2.4 + 1.0 / rate, false);
        s.note(4.3, 0.6, 43);
        s.pedal(4.5, true); s.pedal(4.5 + 1.0 / rate, false);
    }
    else if (name == "local-transpose")
    {
        s.description = "Local keyboard: repeat a low key while changing transpose, then release. The earlier fallback bass must not remain held.";
        p.keyMode = KeyMode::Poly2; p.keyTranspose = -12;
        p.cutoff = 0.7f; p.release = 0.1f;
        s.local(0.1, 36, true);
        auto q = p; q.keyTranspose = 0; s.patch(0.7, q);
        s.local(0.7, 36, true); s.local(1.3, 36, false);
        s.patch(3.1, p); s.local(3.1, 40, true);
        s.patch(3.7, q); s.local(3.7, 40, true); s.local(4.3, 40, false);
    }
    else if (name == "velocity-zero")
    {
        s.description = "Direct timing: short MIDI notes released by velocity-zero Note On. Notes must stop rather than accumulate into a stuck chord.";
        p.cutoff = 0.7f; p.release = 0.1f;
        const std::array<int, 6> notes {48, 55, 60, 50, 57, 62};
        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            const double time = 0.1 + i * 0.8;
            s.events.push_back({static_cast<int>(std::llround(time * rate)), Event::NoteOn, notes[i], 1.0f, {}});
            s.zeroOff(time + 0.4, notes[i]);
        }
    }
    else if (name == "ensemble")
    {
        s.description = "A musical context: warm six-note chord, bright pulsed answering phrase and a final held chorus chord.";
        p.chorus = ChorusMode::One; p.pulseEnabled = true; p.pwmDepth = 0.75f;
        p.pwmSource = PwmSource::Lfo; p.lfoRate = 0.45f;
        p.cutoff = 0.62f; p.resonance = 0.25f; p.envDepth = 0.2f;
        p.attack = 0.07f; p.release = 0.2f; p.subLevel = 0.25f;
        for (int n : {36, 43, 50, 60, 64, 71}) s.note(0.1, 2.0, n);
        const std::array<int, 6> notes {60, 67, 74, 72, 67, 62};
        for (std::size_t i = 0; i < notes.size(); ++i) s.note(2.6 + i * 0.35, 0.22, notes[i]);
        for (int n : {38, 45, 52, 60, 65, 69}) s.note(4.9, 1.8, n);
    }
    else throw std::runtime_error("unknown score: " + name);

    std::stable_sort(s.events.begin(), s.events.end(), [](const Event& a, const Event& b) {
        return a.frame < b.frame;
    });
    return s;
}

// Explicit score-controlled coordinates; every other parameter is the same
// revision's active product profile. That difference belongs to build scope.
void patchJson(std::ostream& out, const EngineParameters& p)
{
    out << "{\"calibration\":" << p.calibration << ",\"aging\":" << p.aging
        << ",\"polyphony\":" << p.polyphony << ",\"key_mode\":" << static_cast<int>(p.keyMode)
        << ",\"saw\":" << p.sawEnabled << ",\"pulse\":" << p.pulseEnabled
        << ",\"range\":" << static_cast<int>(p.range) << ",\"sub\":" << p.subLevel
        << ",\"noise\":" << p.noiseLevel << ",\"pwm_depth\":" << p.pwmDepth
        << ",\"pwm_source\":" << static_cast<int>(p.pwmSource) << ",\"lfo_rate\":" << p.lfoRate
        << ",\"lfo_delay\":" << p.lfoDelay << ",\"cutoff\":" << p.cutoff
        << ",\"resonance\":" << p.resonance << ",\"env_depth\":" << p.envDepth
        << ",\"key_follow\":" << p.keyFollow << ",\"dco_lfo_depth\":" << p.dcoLfoDepth
        << ",\"vcf_lfo_depth\":" << p.vcfLfoDepth << ",\"attack\":" << p.attack
        << ",\"decay\":" << p.decay << ",\"sustain\":" << p.sustain
        << ",\"release\":" << p.release << ",\"vca_level\":" << p.vcaLevel
        << ",\"volume\":" << p.volume << ",\"chorus\":" << static_cast<int>(p.chorus)
        << ",\"high_pass\":" << static_cast<int>(p.highPass)
        << ",\"velocity_depth\":" << p.velocityDepth << ",\"portamento\":" << p.portamento
        << ",\"key_transpose\":" << p.keyTranspose << '}';
}

void render(const std::string& name, const std::filesystem::path& path,
            bool original, int quality)
{
    require(!std::filesystem::exists(path), "output exists; preserve prior evidence");
    const auto score = scoreFor(name);
    auto engine = std::make_unique<YouKnowEngine>();
    ActiveProductFidelityProfile::configureBeforePrepare(*engine);
    require(engine->configureThermalStart(true), "cannot select settled thermal start");
    engine->selectConverterTimingProfile(YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
    engine->prepare(rate, blockSize, quality);
    engine->setParameters(score.initial);
    engine->setOriginalPerformanceMode(original);
    engine->setPitchBend(0.0f); engine->setModWheel(0.0f);
    std::array<float, blockSize> left {}, right {};
    const auto process = [&](int count) {
        engine->process(left.data(), right.data(), count);
        require(!original || engine->originalPerformanceHealthy(), "original firmware execution failed");
    };
    for (int frame = 0; frame < prerollFrames;)
    {
        const int count = std::min(blockSize, prerollFrames - frame);
        process(count); frame += count;
    }
    StereoBuffer audio;
    audio.left.reserve(static_cast<std::size_t>(score.frames));
    audio.right.reserve(static_cast<std::size_t>(score.frames));
    std::size_t next = 0;
    int peakVoices = 0;
    for (int frame = 0; frame < score.frames;)
    {
        while (next < score.events.size() && score.events[next].frame == frame)
        {
            const auto& event = score.events[next++];
            switch (event.kind)
            {
                case Event::NoteOn: engine->noteOn(event.note, event.value); break;
                case Event::NoteOff: engine->noteOff(event.note); break;
                case Event::Pedal: engine->setSustainPedal(event.value != 0.0f); break;
                case Event::Patch: engine->setParameters(event.patch); break;
                case Event::LocalOn: engine->noteOnFromLocalKeyboard(event.note, event.value); break;
                case Event::LocalOff: engine->noteOffFromLocalKeyboard(event.note); break;
            }
        }
        int count = std::min(blockSize, score.frames - frame);
        if (next < score.events.size()) count = std::min(count, score.events[next].frame - frame);
        require(count > 0, "invalid score schedule");
        process(count);
        peakVoices = std::max(peakVoices, engine->getActiveVoiceCount());
        audio.left.insert(audio.left.end(), left.begin(), left.begin() + count);
        audio.right.insert(audio.right.end(), right.begin(), right.begin() + count);
        frame += count;
    }
    require(next == score.events.size(), "score contains events after render end");
    std::string error;
    require(validate(audio, error), error);
    const auto level = measure(audio);
    require(level.rms > 1e-7 && peakVoices > 0, "silent score");
    require(writeFloatWav(path, audio, error, rate), error);
    std::cout << std::setprecision(17)
        << "{\"protocol\":\"youknow-fidelity-2026-10-15-v1\",\"score\":" << std::quoted(name)
        << ",\"description\":" << std::quoted(score.description)
        << ",\"sample_rate\":" << rate << ",\"channels\":2,\"frames\":" << score.frames
        << ",\"max_block_size\":" << blockSize << ",\"preroll_frames\":" << prerollFrames
        << ",\"quality_requested\":" << quality << ",\"quality_applied\":" << engine->getOversamplingFactor()
        << ",\"timing\":" << std::quoted(original ? "original" : "direct")
        << ",\"thermal_start\":\"settled\",\"seed_policy\":\"fixed engine reset/card/noise seeds\""
        << ",\"profile\":\"ActiveProductFidelityProfile\",\"kernel\":\"PolyZoned/Cubic/Rk4Single\""
        << ",\"peak\":" << level.peak << ",\"rms\":" << level.rms
        << ",\"peak_voices\":" << peakVoices << ",\"final_active_voices\":" << engine->getActiveVoiceCount()
        << ",\"patch\":";
    patchJson(std::cout, score.initial);
    std::cout << ",\"events\":[";
    bool first = true;
    for (const auto& event : score.events)
    {
        if (!first) std::cout << ',';
        first = false;
        constexpr std::array<const char*, 6> kinds {"on", "off", "pedal", "patch", "local-on", "local-off"};
        std::cout << "{\"frame\":" << event.frame << ",\"kind\":" << std::quoted(kinds[event.kind]);
        if (event.kind == Event::Patch) { std::cout << ",\"patch\":"; patchJson(std::cout, event.patch); }
        else std::cout << ",\"note\":" << event.note << ",\"value\":" << event.value;
        std::cout << '}';
    }
    std::cout << "]}\n";
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--list")
        {
            for (const auto* name : names) std::cout << name << '\n';
            return 0;
        }
        require(argc >= 4 && argc <= 6 && std::string(argv[1]) == "--render",
            "usage: YouKnowRenderFidelityQuickWins --render SCORE OUTPUT.wav [original|direct] [1|2|4]; or --list");
        const std::string timing = argc > 4 ? argv[4] : "original";
        const std::string quality = argc > 5 ? argv[5] : "1";
        require(timing == "original" || timing == "direct", "timing must be original or direct");
        require(quality == "1" || quality == "2" || quality == "4", "quality must be 1, 2 or 4");
        render(argv[2], std::filesystem::absolute(argv[3]), timing == "original", quality[0] - '0');
    }
    catch (const std::exception& error)
    {
        std::cerr << "RenderFidelityQuickWins: " << error.what() << '\n';
        return 1;
    }
}
