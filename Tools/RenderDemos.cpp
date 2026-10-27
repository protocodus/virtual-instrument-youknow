// Renders six technique takes to a local directory, from the same JUCE-free
// engine the plug-in runs, so a take cannot drift away from what YouKnow
// actually sounds like. They are listening material for development, not
// committed: the README's demos are the preset and More demos
// RenderPresetDemos.cpp plays from Docs/audio/midi (G3, G3d). No samples or
// external processing are involved anywhere: every file here comes out of the
// circuit model, including the chorus hiss, which is part of the instrument.
//
//   cmake --build <build-dir> --target YouKnowRenderDemos
//   <build-dir>/YouKnowRenderDemos <output-directory>

#include "DemoPanels.h"
#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowActiveProductFidelity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
using youknow::ChorusMode;
using youknow::DcoRange;
using youknow::EngineParameters;
using youknow::EnvPolarity;
using youknow::HighPassMode;
using youknow::PwmSource;
using youknow::VcaMode;
using youknow::YouKnowEngine;
using youknow::demo_panels::plainPanel;

// 44.1 kHz 16-bit is what a listener's browser, phone and DAW all handle
// without conversion, and it is the rate the instrument is most often used at.
constexpr double demoSampleRate = 44100.0;
constexpr int renderBlockSize = 256;
// -3 dBFS: loud enough to audition without a gain change between files, with
// headroom left so no 16-bit sample sits against full scale.
constexpr double normalisedPeak = 0.7079457843841379;
// These are file-delivery guards rather than synthesizer transfer values. A
// demo with more DC can waste headroom or thump downstream equipment, while a
// large non-zero endpoint can click when a player starts or stops the file.
constexpr double maximumAbsoluteDcMean = 0.005;
constexpr double maximumEdgeMagnitude = 0.01;

// ---------------------------------------------------------------------------
// WAV output
// ---------------------------------------------------------------------------

void appendLittleEndian (std::vector<std::uint8_t>& bytes, std::uint32_t value,
                         int byteCount)
{
    for (int index = 0; index < byteCount; ++index)
        bytes.push_back (static_cast<std::uint8_t> ((value >> (8 * index)) & 0xffu));
}

// YouKnow is a stereo instrument by construction - its two channels are the
// two chorus lines clocked in antiphase - so every take is written as a stereo
// file even when the effect is off and the image collapses.
bool writeWav (const std::filesystem::path& path, const std::vector<float>& left,
               const std::vector<float>& right)
{
    const auto frames = static_cast<std::uint32_t> (left.size());
    constexpr std::uint16_t channels = 2u;
    const std::uint32_t byteRate =
        static_cast<std::uint32_t> (demoSampleRate) * channels * 2u;
    const std::uint32_t dataBytes = frames * channels * 2u;

    std::vector<std::uint8_t> bytes;
    bytes.reserve (44u + dataBytes);
    const auto tag = [&bytes] (const char* text)
    {
        for (int index = 0; index < 4; ++index)
            bytes.push_back (static_cast<std::uint8_t> (text[index]));
    };
    tag ("RIFF");
    appendLittleEndian (bytes, 36u + dataBytes, 4);
    tag ("WAVE");
    tag ("fmt ");
    appendLittleEndian (bytes, 16u, 4);
    appendLittleEndian (bytes, 1u, 2); // PCM
    appendLittleEndian (bytes, channels, 2);
    appendLittleEndian (bytes, static_cast<std::uint32_t> (demoSampleRate), 4);
    appendLittleEndian (bytes, byteRate, 4);
    appendLittleEndian (bytes, channels * 2u, 2); // block align
    appendLittleEndian (bytes, 16u, 2);           // bits per sample
    tag ("data");
    appendLittleEndian (bytes, dataBytes, 4);

    const auto encode = [] (float value)
    {
        if (! std::isfinite (value))
            value = 0.0f;
        const float clamped = std::clamp (value, -1.0f, 1.0f);
        // Symmetric scaling by 32767 so a full-scale negative peak cannot wrap.
        const auto sample =
            static_cast<std::int32_t> (std::lround (static_cast<double> (clamped) * 32767.0));
        return static_cast<std::uint32_t> (
            static_cast<std::uint16_t> (static_cast<std::int16_t> (sample)));
    };

    for (std::size_t frame = 0; frame < left.size(); ++frame)
    {
        appendLittleEndian (bytes, encode (left[frame]), 2);
        appendLittleEndian (bytes, encode (right[frame]), 2);
    }

    std::FILE* file = std::fopen (path.string().c_str(), "wb");
    if (file == nullptr)
        return false;
    const bool written =
        std::fwrite (bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose (file);
    return written;
}

// ---------------------------------------------------------------------------
// A take: an imperative score driving the engine
// ---------------------------------------------------------------------------

class Take
{
public:
    // The engine is held indirectly because it is large; a take, on the other
    // hand, is returned by value from every score below.
    explicit Take (EngineParameters parameters)
        : engine_ (std::make_unique<YouKnowEngine>())
    {
        youknow::ActiveProductFidelityProfile::configureBeforePrepare (*engine_);
        // The product's converter placement (Docs/decisions.md, 2026-09-04),
        // selected exactly as the plug-in selects it before prepare().
        engine_->selectConverterTimingProfile (
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine_->prepare (demoSampleRate, renderBlockSize, true);
        setParameters (parameters);
    }

    void setParameters (EngineParameters parameters)
    {
        youknow::ActiveProductFidelityProfile::applyTo (parameters);
        engine_->setParameters (parameters);
    }

    void on (int note, float velocity = 1.0f) { engine_->noteOn (note, velocity); }
    void off (int note) { engine_->noteOff (note); }
    void bend (float amount) { engine_->setPitchBend (amount); }
    void wheel (float amount) { engine_->setModWheel (amount); }

    // Strike a key, hold it for `holdSeconds`, release it, then let it ring
    // for `gapSeconds` before returning.
    void hit (int note, float velocity, double holdSeconds, double gapSeconds)
    {
        on (note, velocity);
        rest (holdSeconds);
        off (note);
        rest (gapSeconds);
    }

    // A block chord: every note down together, held, then released together.
    void chord (std::initializer_list<int> notes, float velocity,
                double holdSeconds, double gapSeconds)
    {
        for (const int note : notes)
            on (note, velocity);
        rest (holdSeconds);
        for (const int note : notes)
            off (note);
        rest (gapSeconds);
    }

    void rest (double seconds) { render (seconds); }

    [[nodiscard]] const std::vector<float>& left() const noexcept { return left_; }
    [[nodiscard]] const std::vector<float>& right() const noexcept { return right_; }

    [[nodiscard]] bool finite() const noexcept
    {
        for (std::size_t index = 0; index < left_.size(); ++index)
            if (! std::isfinite (left_[index]) || ! std::isfinite (right_[index]))
                return false;
        return true;
    }

    [[nodiscard]] double peak() const noexcept
    {
        double result = 0.0;
        for (std::size_t index = 0; index < left_.size(); ++index)
            result = std::max ({ result, std::abs (static_cast<double> (left_[index])),
                                 std::abs (static_cast<double> (right_[index])) });
        return result;
    }

    [[nodiscard]] double absoluteDcMean() const noexcept
    {
        if (left_.empty())
            return 0.0;

        double leftSum = 0.0;
        double rightSum = 0.0;
        for (std::size_t index = 0; index < left_.size(); ++index)
        {
            leftSum += static_cast<double> (left_[index]);
            rightSum += static_cast<double> (right_[index]);
        }
        const auto frames = static_cast<double> (left_.size());
        return std::max (std::abs (leftSum / frames),
                         std::abs (rightSum / frames));
    }

    [[nodiscard]] double firstEdgeMagnitude() const noexcept
    {
        if (left_.empty())
            return 0.0;
        return std::max (std::abs (static_cast<double> (left_.front())),
                         std::abs (static_cast<double> (right_.front())));
    }

    [[nodiscard]] double lastEdgeMagnitude() const noexcept
    {
        if (left_.empty())
            return 0.0;
        return std::max (std::abs (static_cast<double> (left_.back())),
                         std::abs (static_cast<double> (right_.back())));
    }

    // Brings the take to a common listening level. The returned value is the
    // gain applied, so the manifest can record what each file needed.
    double normalise()
    {
        const auto current = peak();
        if (current <= 1.0e-9)
            return 1.0;
        const auto gain = normalisedPeak / current;
        for (std::size_t index = 0; index < left_.size(); ++index)
        {
            left_[index] = static_cast<float> (left_[index] * gain);
            right_[index] = static_cast<float> (right_[index] * gain);
        }
        return gain;
    }

private:
    void render (double seconds)
    {
        auto remaining =
            static_cast<int> (std::lround (seconds * demoSampleRate));
        std::array<float, renderBlockSize> blockLeft {};
        std::array<float, renderBlockSize> blockRight {};

        while (remaining > 0)
        {
            const int count = std::min (renderBlockSize, remaining);
            engine_->process (blockLeft.data(), blockRight.data(), count);
            left_.insert (left_.end(), blockLeft.begin(), blockLeft.begin() + count);
            right_.insert (right_.end(), blockRight.begin(), blockRight.begin() + count);
            remaining -= count;
        }
    }

    std::unique_ptr<YouKnowEngine> engine_;
    std::vector<float> left_;
    std::vector<float> right_;
};

// Every take starts from demo_panels::plainPanel() and is normalised to a
// common file level afterward; the unnormalised peak is reported so a
// signal-path gain change remains visible during review.

// ---------------------------------------------------------------------------
// The takes
// ---------------------------------------------------------------------------

// The classic chorus pad: saw and sub through a half-open filter with a slow
// attack, the effect in mode I. The hiss is the delay lines' own.
Take renderChorusPad()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.7f;
    parameters.vcaLevel = 0.84f;
    parameters.cutoff = 0.48f;
    parameters.envDepth = 0.25f;
    parameters.attack = 0.35f;
    parameters.sustain = 0.85f;
    parameters.release = 0.55f;
    parameters.chorus = ChorusMode::One;
    Take take (parameters);

    take.rest (0.10);
    take.chord ({ 48, 55, 60, 64 }, 0.9f, 3.6, 1.2);
    take.chord ({ 46, 53, 58, 62 }, 0.9f, 3.6, 1.2);
    take.chord ({ 44, 51, 60, 63 }, 0.9f, 3.6, 1.0);
    take.chord ({ 43, 50, 59, 62 }, 0.9f, 4.2, 3.4);
    return take;
}

// Pulse-width-modulated strings in chorus II. B-2 computes PWM directly from
// the raw LFO accumulator; its onset delay applies only to pitch and filter.
Take renderPwmStrings()
{
    auto parameters = plainPanel();
    parameters.sawEnabled = false;
    parameters.pulseEnabled = true;
    parameters.pwmSource = PwmSource::Lfo;
    parameters.pwmDepth = 0.75f;
    parameters.vcaLevel = 0.84f;
    parameters.lfoRate = 0.35f;
    parameters.cutoff = 0.55f;
    parameters.attack = 0.25f;
    parameters.sustain = 0.9f;
    parameters.release = 0.45f;
    parameters.chorus = ChorusMode::Two;
    Take take (parameters);

    take.rest (0.10);
    take.chord ({ 57, 60, 64 }, 0.9f, 3.0, 0.9);
    take.chord ({ 55, 59, 62 }, 0.9f, 3.0, 0.9);
    take.chord ({ 53, 57, 60, 65 }, 0.9f, 4.4, 3.0);
    return take;
}

// The 16' bass: saw and sub with a snappy filter envelope. The falling
// segments are the generator's own exponentials through the quasi-linear
// amplifier, which is where the punch of this instrument's envelopes lives.
Take renderBassline()
{
    auto parameters = plainPanel();
    parameters.range = DcoRange::Sixteen;
    parameters.vcaLevel = 0.84f;
    parameters.subLevel = 0.85f;
    parameters.cutoff = 0.34f;
    parameters.resonance = 0.30f;
    parameters.envDepth = 0.45f;
    parameters.attack = 0.0f;
    parameters.decay = 0.32f;
    parameters.sustain = 0.25f;
    parameters.release = 0.18f;
    Take take (parameters);

    take.rest (0.10);
    const std::array<std::pair<int, double>, 16> line {{
        { 48, 0.22 }, { 48, 0.22 }, { 60, 0.11 }, { 48, 0.22 },
        { 51, 0.22 }, { 48, 0.22 }, { 46, 0.11 }, { 48, 0.22 },
        { 48, 0.22 }, { 48, 0.22 }, { 60, 0.11 }, { 48, 0.22 },
        { 53, 0.22 }, { 51, 0.22 }, { 46, 0.11 }, { 44, 0.44 },
    }};
    for (int repeat = 0; repeat < 2; ++repeat)
        for (const auto& [note, gap] : line)
            take.hit (note, 0.95f, 0.16, gap);
    take.rest (2.0);
    return take;
}

// One pad through all four live effect states: off, I, II, I+II. Mode II is
// faster, not deeper; I+II runs the established summed-rate product mode, and
// the lines' own hiss rides underneath every wet state.
Take renderChorusModes()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.6f;
    parameters.vcaLevel = 0.84f;
    parameters.cutoff = 0.52f;
    parameters.attack = 0.12f;
    parameters.sustain = 0.9f;
    parameters.release = 0.35f;
    Take take (parameters);

    take.rest (0.10);
    for (const ChorusMode mode :
         { ChorusMode::Off, ChorusMode::One, ChorusMode::Two,
           ChorusMode::OneTwo })
    {
        parameters.chorus = mode;
        take.setParameters (parameters);
        take.chord ({ 52, 59, 64, 68 }, 0.9f, 3.4, 1.3);
    }
    take.rest (1.6);
    return take;
}

// The four-position high-pass switch on one bright chord: the measured bass
// boost, the flat leg, and the two cutting corners.
Take renderHighPassLadder()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.8f;
    parameters.vcaLevel = 0.84f;
    parameters.cutoff = 0.66f;
    parameters.attack = 0.02f;
    parameters.sustain = 0.9f;
    parameters.release = 0.25f;
    Take take (parameters);

    take.rest (0.10);
    for (const HighPassMode mode :
         { HighPassMode::Boost, HighPassMode::One, HighPassMode::Two,
           HighPassMode::Three })
    {
        parameters.highPass = mode;
        take.setParameters (parameters);
        take.chord ({ 36, 48, 55, 60, 64 }, 0.9f, 1.7, 0.55);
    }
    take.rest (1.5);
    return take;
}

// One chord at the nominal zero-character baseline, then with the optional
// voiced Unit Character profile at full amount. The latter is a repeatable
// sound-design policy, not a measured worn-unit population.
Take renderUnitCharacter()
{
    auto parameters = plainPanel();
    parameters.subLevel = 0.55f;
    parameters.vcaLevel = 0.84f;
    parameters.cutoff = 0.42f;
    parameters.resonance = 0.55f;
    parameters.envDepth = 0.3f;
    parameters.attack = 0.08f;
    parameters.sustain = 0.85f;
    parameters.release = 0.4f;
    Take take (parameters);

    take.rest (0.10);
    for (const float calibration : { 0.0f, 1.0f })
    {
        parameters.calibration = calibration;
        take.setParameters (parameters);
        take.chord ({ 45, 52, 57, 61, 64, 69 }, 0.9f, 4.2, 1.3);
    }
    take.rest (1.8);
    return take;
}

// ---------------------------------------------------------------------------
// The demo table
// ---------------------------------------------------------------------------

struct Demo
{
    const char* fileName;
    const char* description;
    Take (*render)();
};

const std::array<Demo, 6>& demos()
{
    static const std::array<Demo, 6> table {{
        { "01-chorus-pad.wav",
          "Saw and sub through the mode-I chorus: the classic pad, hiss and all",
          renderChorusPad },
        { "02-pwm-strings.wav",
          "Pulse-width-modulated strings in the faster mode-II chorus",
          renderPwmStrings },
        { "03-sixteen-foot-bass.wav",
          "A 16' bassline: the exponential envelope segments doing the punch",
          renderBassline },
        { "04-chorus-modes.wav",
          "The same pad with the effect off, in mode I, mode II, then I+II",
          renderChorusModes },
        { "05-high-pass-ladder.wav",
          "One bright chord through all four high-pass switch positions",
          renderHighPassLadder },
        { "06-unit-character.wav",
          "A six-voice chord at nominal zero Unit Character, then at full amount",
          renderUnitCharacter },
    }};
    return table;
}

// A short render used by the regression suite: it proves the tool and the
// engine still produce finite, audible audio and a readable WAV without
// committing anything.
int runSmokeTest (const std::filesystem::path& directory)
{
    auto parameters = plainPanel();
    parameters.attack = 0.0f;
    Take take (parameters);
    take.hit (60, 0.95f, 0.30, 0.25);

    if (! take.finite())
    {
        std::fprintf (stderr, "smoke test: rendered a non-finite sample\n");
        return 1;
    }
    if (take.peak() < 1.0e-3)
    {
        std::fprintf (stderr, "smoke test: rendered silence (peak %.6f)\n", take.peak());
        return 1;
    }

    std::error_code error;
    std::filesystem::create_directories (directory, error);
    const auto path = directory / "smoke.wav";
    if (! writeWav (path, take.left(), take.right()))
    {
        std::fprintf (stderr, "smoke test: could not write %s\n", path.string().c_str());
        return 1;
    }
    const auto size = std::filesystem::file_size (path, error);
    std::filesystem::remove (path, error);
    if (size < 44u + take.left().size() * 4u)
    {
        std::fprintf (stderr, "smoke test: short WAV (%llu bytes)\n",
                      static_cast<unsigned long long> (size));
        return 1;
    }

    std::printf ("YouKnow demo renderer smoke test passed (peak %.3f).\n",
                 take.peak());
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    const char* usage = "usage: YouKnowRenderDemos [--smoke] output-directory\n";
    std::vector<std::string> arguments (argv + 1, argv + argc);
    bool smoke = false;
    std::filesystem::path directory;

    for (const auto& argument : arguments)
    {
        if (argument == "--smoke")
        {
            smoke = true;
        }
        else if (argument == "--help" || argument == "-h")
        {
            std::printf ("%s", usage);
            return 0;
        }
        else if (directory.empty())
        {
            directory = argument;
        }
        else
        {
            std::fprintf (stderr, "%s", usage);
            return 1;
        }
    }

    // The takes are local listening material, never committed (README G3d),
    // so there is no default directory to write into by accident.
    if (directory.empty())
    {
        std::fprintf (stderr, "%s", usage);
        return 1;
    }
    if (smoke)
        return runSmokeTest (directory);

    std::error_code error;
    std::filesystem::create_directories (directory, error);
    if (! std::filesystem::is_directory (directory))
    {
        std::fprintf (stderr, "not a directory: %s\n", directory.string().c_str());
        return 1;
    }

    for (const auto& demo : demos())
    {
        auto take = demo.render();

        if (! take.finite())
        {
            std::fprintf (stderr, "%s rendered a non-finite sample\n", demo.fileName);
            return 1;
        }

        const auto renderedPeak = take.peak();
        if (renderedPeak < 1.0e-4)
        {
            std::fprintf (stderr, "%s rendered silence (peak %.6f)\n", demo.fileName,
                          renderedPeak);
            return 1;
        }
        const auto gain = take.normalise();
        const auto absoluteDcMean = take.absoluteDcMean();
        const auto firstEdge = take.firstEdgeMagnitude();
        const auto lastEdge = take.lastEdgeMagnitude();
        if (absoluteDcMean > maximumAbsoluteDcMean)
        {
            std::fprintf (stderr,
                          "%s rejected: normalised absolute DC mean %.6f FS "
                          "exceeds %.6f FS\n",
                          demo.fileName, absoluteDcMean, maximumAbsoluteDcMean);
            return 1;
        }
        if (firstEdge > maximumEdgeMagnitude || lastEdge > maximumEdgeMagnitude)
        {
            std::fprintf (stderr,
                          "%s rejected: normalised first/last edge "
                          "%.6f/%.6f FS exceeds %.6f FS\n",
                          demo.fileName, firstEdge, lastEdge,
                          maximumEdgeMagnitude);
            return 1;
        }

        const auto path = directory / demo.fileName;
        if (! writeWav (path, take.left(), take.right()))
        {
            std::fprintf (stderr, "could not write %s\n", path.string().c_str());
            return 1;
        }

        std::printf ("Rendered %-28s %5.1f s  rendered peak %6.1f dBFS  normalised %+5.1f dB  %s\n",
                     demo.fileName, static_cast<double> (take.left().size()) / demoSampleRate,
                     20.0 * std::log10 (renderedPeak), 20.0 * std::log10 (gain),
                     demo.description);
    }

    std::printf ("Rendered %zu takes into %s\n", demos().size(),
                 directory.string().c_str());
    return 0;
}
