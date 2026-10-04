// Deterministic electronics review: render dir label revision [1|4], compare dir A B.
// render-next/compare-next select the 2026-10-04 continuation score; both
// builds use the existing Phones route. Never overwrite a previous review.
// Compile this IDENTICAL score against the archived baseline and final DSP.
// Feature guards omit APIs absent at baseline; the only changed connection is
// the explicitly requested headphone route (Line at baseline, Phones at final).
// Each take trims declared latency, uses fixed reset seeds, settled thermal
// state, 48 kHz / 128-frame event-split blocks and the product signal path.
// Raw float audio is immutable; PCM24 listening copies use whole-file stereo
// RMS matching and common headroom. The signed difference is after minus before.
// Noise-only level changes should also be assessed through raw metrics because
// RMS matching intentionally removes a broadband loudness difference.

#include "DSP/YouKnowEngine.h"
#include "DSP/YouKnowProductFidelity.h"
#include "RealismComparisonSupport.h"

#include <bit>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifndef YOUKNOW_DSP_SOURCE_SHA256
#define YOUKNOW_DSP_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_ID
#define YOUKNOW_COMPARISON_COMPILER_ID "unknown"
#endif
#ifndef YOUKNOW_COMPARISON_COMPILER_VERSION
#define YOUKNOW_COMPARISON_COMPILER_VERSION "unknown"
#endif
#ifndef YOUKNOW_ELECTRONICS_SOURCE_SHA256
#define YOUKNOW_ELECTRONICS_SOURCE_SHA256 "unavailable"
#endif
#ifndef YOUKNOW_ELECTRONICS_HAVE_RESONANCE
#define YOUKNOW_ELECTRONICS_HAVE_RESONANCE 0
#endif
#ifndef YOUKNOW_ELECTRONICS_HAVE_OTA
#define YOUKNOW_ELECTRONICS_HAVE_OTA 0
#endif
#ifndef YOUKNOW_ELECTRONICS_HAVE_PHONES
#define YOUKNOW_ELECTRONICS_HAVE_PHONES 0
#endif

namespace
{
using namespace youknow;
using namespace youknow::tools::realism;
constexpr std::size_t sectionCount = 7;
std::array<std::string_view, sectionCount> slugs {
    "01-resonance", "02-ota-breath", "03-chorus-switch",
    "04-phones-32ohm", "05-bbd-hiss", "06-musical", "montage"
};
std::string_view protocol = "youknow-electronics-v1";
bool continuationScore = false;
void selectContinuationScore()
{
    continuationScore = true;
    protocol = "youknow-electronics-continuation-v1";
    slugs = { "01-resonance", "02-voice-tail", "03-chorus-switch",
              "04-phones-bright", "05-phones-floor", "06-musical", "montage" };
}
std::array<int, slugs.size()> reportedLatency {};
std::string capabilities()
{
    return std::string(YOUKNOW_ELECTRONICS_HAVE_RESONANCE ? "1" : "0")
         + (YOUKNOW_ELECTRONICS_HAVE_OTA ? "1" : "0")
         + (YOUKNOW_ELECTRONICS_HAVE_PHONES ? "1" : "0");
}

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string safeLabel(std::string_view value)
{
    require(!value.empty() && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")
        == std::string_view::npos, "labels must contain only letters, numbers, -_.");
    require(value != "." && value != "..", "invalid label");
    return std::string(value);
}

std::string hashAudio(const StereoBuffer& audio)
{
    // FNV-1a over little-endian float32 samples: corruption/identity binding,
    // not a cryptographic substitute for the build's SHA-256 source identity.
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
    std::ostringstream result;
    result << std::hex << std::setw(16) << std::setfill('0') << hash;
    return result.str();
}

EngineParameters panel()
{
    EngineParameters p;
    ProductFidelityProfile::applyTo(p);
#if YOUKNOW_ELECTRONICS_HAVE_RESONANCE
    require(p.enableResonanceSoftJunction, "product profile omitted Tr18 junction");
#endif
#if YOUKNOW_ELECTRONICS_HAVE_OTA
    require(p.enableOtaShotNoise, "product profile omitted OTA collector noise");
#endif
#if YOUKNOW_ELECTRONICS_HAVE_PHONES
    require(p.enableVoiceVcaAntialias, "product profile omitted VCA antialiasing");
#endif
    p.vcfTanhMode = VcfTanhMode::PolyZoned;
    p.vcfFastEarlyMode = VcfFastEarlyMode::Cubic;
    p.vcfSolverMode = VcfSolverMode::Rk4Single;
    p.calibration = 1.0f;
    p.aging = 0.5f;
    p.volume = 0.6f;
    p.velocityDepth = 0.0f;
    p.polyphony = 6;
    p.highPass = HighPassMode::One;
    p.vcaLevel = 0.85f;
    p.chorus = ChorusMode::Off;
    return p;
}

class Performance
{
public:
    Performance(EngineParameters p, int quality, bool original = false)
        : engine_(std::make_unique<YouKnowEngine>())
    {
        ProductFidelityProfile::configureBeforePrepare(*engine_);
        require(engine_->configureThermalStart(true), "cannot configure settled thermal start");
        engine_->selectConverterTimingProfile(
            YouKnowEngine::ConverterTimingProfile::MeasuredChartGeometry);
        engine_->setParameters(p);
        (void) original;
        engine_->prepare(comparisonSampleRate, comparisonBlockSize, quality);
        latency_ = engine_->getProcessingLatencySamples();
        require(latency_ >= 0 && latency_ < 4096, "invalid declared processing latency");
        run(0.25, false);
    }

    void run(double seconds, bool retain = true)
    {
        auto remaining = static_cast<int>(std::llround(seconds * comparisonSampleRate));
        while (remaining > 0)
        {
            const int count = std::min(remaining, comparisonBlockSize);
            engine_->process(left_.data(), right_.data(), count);
            if (retain)
            {
                audio_.left.insert(audio_.left.end(), left_.begin(), left_.begin() + count);
                audio_.right.insert(audio_.right.end(), right_.begin(), right_.begin() + count);
            }
            remaining -= count;
        }
    }

    void on(int note) { engine_->noteOn(note, 1.0f); }
    void off(int note) { engine_->noteOff(note); }
    void set(const EngineParameters& p) { engine_->setParameters(p); }
    void hit(int note, double hold, double gap)
    {
        on(note); run(hold); off(note); run(gap);
    }
    void chord(std::initializer_list<int> notes, double hold, double gap)
    {
        for (const int note : notes) on(note);
        run(hold);
        for (const int note : notes) off(note);
        run(gap);
    }
    void hold(bool value) { engine_->setSustainPedal(value); }
    StereoBuffer take(std::size_t section)
    {
        require(section < reportedLatency.size(), "invalid scene index");
        // Pull the complete delayed tail, then remove the declared leading
        // latency. MIDI and serial timing differences remain in the audio.
        run(double(latency_) / comparisonSampleRate);
        require(audio_.left.size() > std::size_t(latency_), "scene shorter than latency");
        audio_.left.erase(audio_.left.begin(), audio_.left.begin() + latency_);
        audio_.right.erase(audio_.right.begin(), audio_.right.begin() + latency_);
        reportedLatency[section] = latency_;
        return std::move(audio_);
    }

private:
    std::unique_ptr<YouKnowEngine> engine_;
    std::array<float, comparisonBlockSize> left_ {}, right_ {};
    StereoBuffer audio_;
    int latency_ {};
};

StereoBuffer resonance(int quality)
{
    auto p = panel();
    p.sawEnabled = false; p.pulseEnabled = false; p.subLevel = 0;
    p.noiseLevel = .10f; p.cutoff = .47f; p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate; p.chorusNoise = 0; p.resonance = 0;
    Performance take(p, quality);
    take.on(60);
    for (const float res : {0.f, .025f, .045f, .10f, .30f, .65f}) {
        p.resonance = res; take.set(p); take.run(.5);
    }
    take.off(60); take.run(.2);
    return take.take(0);
}

StereoBuffer otaBreath(int quality)
{
    auto p = panel();
    p.cutoff = .29f; p.resonance = .76f; p.envDepth = 0; p.keyFollow = 0;
    p.attack = .05f; p.decay = .16f; p.sustain = .006f;
    p.release = YouKnowEngine::panelPositionForRelease(.65f);
    p.chorusNoise = 0; p.noiseLevel = 0;
    Performance take(p, quality);
    take.hit(48, .95, .85);
    take.hit(55, .95, .85);
    return take.take(1);
}

StereoBuffer chorusSwitch(int quality)
{
    auto p = panel();
    p.cutoff = .68f; p.resonance = .13f; p.envDepth = 0;
    p.vcaMode = VcaMode::Gate; p.chorusNoise = .25f;
    Performance take(p, quality);
    take.on(48); take.on(55); take.on(60);
    for (const auto mode : {ChorusMode::One, ChorusMode::Off,
                           ChorusMode::Two, ChorusMode::Off}) {
        p.chorus = mode; take.set(p); take.run(.85);
    }
    take.off(48); take.off(55); take.off(60); take.run(.2);
    return take.take(2);
}

StereoBuffer phones(int quality)
{
    auto p = panel();
#if YOUKNOW_ELECTRONICS_HAVE_PHONES
    p.outputRoute = HeadphoneOutput::Route::Headphones;
    p.headphoneLoadOhms = 32.f;
#endif
    p.highPass = HighPassMode::One; p.cutoff = 1; p.resonance = p.envDepth = p.keyFollow = 0;
    p.vcaMode = VcaMode::Gate; p.chorusNoise = 0;
    p.subLevel = .45f; p.pulseEnabled = true;
    Performance take(p, quality);
    // The extended MIDI adapter reaches 20.6 Hz; the second note is within
    // the original keyboard range, so both deep coupling and normal bass are heard.
    if (continuationScore) {
        // Same 32-ohm electrical route in both builds. Upper-register pulse
        // harmonics expose finite amplifier bandwidth without changing loads.
        take.hit(60, 1.4, .15); take.hit(84, 1.4, .25);
    } else {
        take.hit(16, 1.4, .15); take.hit(36, 1.4, .25);
    }
    return take.take(3);
}

StereoBuffer bbdHiss(int quality)
{
    auto p = panel();
    p.sawEnabled = p.pulseEnabled = false; p.subLevel = p.noiseLevel = 0;
    p.chorus = continuationScore ? ChorusMode::Off : ChorusMode::One;
    p.chorusNoise = continuationScore ? 0.f : .75f;
#if YOUKNOW_ELECTRONICS_HAVE_PHONES
    if (continuationScore) {
        p.outputRoute = HeadphoneOutput::Route::Headphones;
        p.headphoneLoadOhms = 32.f;
        p.volume = 0.f; // IC7's intrinsic floor remains after VR1 is grounded.
    }
#endif
    Performance take(p, quality);
    take.run(1.5, false); // settle the physical output/chorus stores
    take.run(4.0);
    return take.take(4);
}

StereoBuffer musical(int quality)
{
    auto p = panel();
    p.pulseEnabled = true; p.pwmSource = PwmSource::Lfo; p.pwmDepth = .25f;
    p.subLevel = .25f; p.cutoff = .52f; p.resonance = .36f;
    p.envDepth = .28f; p.attack = YouKnowEngine::panelPositionForAttack(.015f);
    p.decay = YouKnowEngine::panelPositionForDecay(.35f); p.sustain = .40f;
    p.release = YouKnowEngine::panelPositionForRelease(.35f);
    p.chorus = ChorusMode::One;
    Performance take(p, quality);
    take.chord({48,55,60,64}, .7, .15);
    take.chord({41,48,53,57}, .7, .15);
    take.chord({43,50,55,59}, .7, .15);
    take.chord({48,55,60,64}, .7, .4);
    return take.take(5);
}

void append(StereoBuffer& destination, const StereoBuffer& source)
{
    destination.left.insert(destination.left.end(), source.left.begin(), source.left.end());
    destination.right.insert(destination.right.end(), source.right.begin(), source.right.end());
}

void silence(StereoBuffer& destination, double seconds)
{
    const auto frames = static_cast<std::size_t>(std::llround(seconds * comparisonSampleRate));
    destination.left.insert(destination.left.end(), frames, 0.0f);
    destination.right.insert(destination.right.end(), frames, 0.0f);
}

void writeAudio(const std::filesystem::path& path, const StereoBuffer& audio)
{
    std::string error;
    require(validate(audio, error) && writeFloatWav(path, audio, error), error);
}

std::vector<std::uint8_t> playbackBytes(const StereoBuffer& audio)
{
    // Native players commonly reject IEEE float WAVs. Raw provenance remains
    // float32; review copies use ordinary signed PCM24 without dither or gain.
    std::string error;
    require(validate(audio, error), error);
    require(measure(audio).peak <= 1.0, "PCM24 playback would clip");
    require(audio.left.size() <= (std::numeric_limits<std::uint32_t>::max() - 36u) / 6u,
            "PCM24 playback exceeds RIFF size");
    std::vector<std::uint8_t> bytes;
    const auto dataBytes = static_cast<std::uint32_t>(audio.left.size() * 6u);
    bytes.reserve(44u + dataBytes);
    const auto tag = [&](const char* value) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value[i]));
    };
    tag("RIFF"); appendLittleEndian(bytes, 36u + dataBytes, 4); tag("WAVE"); tag("fmt ");
    appendLittleEndian(bytes, 16u, 4); appendLittleEndian(bytes, 1u, 2);
    appendLittleEndian(bytes, 2u, 2); appendLittleEndian(bytes, comparisonSampleRate, 4);
    appendLittleEndian(bytes, comparisonSampleRate * 6u, 4);
    appendLittleEndian(bytes, 6u, 2); appendLittleEndian(bytes, 24u, 2);
    tag("data"); appendLittleEndian(bytes, dataBytes, 4);
    for (std::size_t i = 0; i < audio.left.size(); ++i)
        for (const float value : {audio.left[i], audio.right[i]})
        {
            const auto sample = static_cast<std::int32_t>(std::llround(double(value) * 8388607.0));
            appendLittleEndian(bytes, static_cast<std::uint32_t>(sample), 3);
        }
    return bytes;
}

void writePlayback(const std::filesystem::path& path, const StereoBuffer& audio)
{
    const auto bytes = playbackBytes(audio);
    std::error_code fsError;
    std::filesystem::create_directories(path.parent_path(), fsError);
    require(!fsError, "cannot create playback directory");
    std::ofstream stream(path, std::ios::binary);
    require(bool(stream), "cannot open PCM24 playback file");
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(bool(stream), "cannot write PCM24 playback file");
}

StereoBuffer readAudio(const std::filesystem::path& path)
{
    StereoBuffer audio;
    std::string error;
    require(readFloatWav(path, audio, error), error);
    return audio;
}

std::string levelJson(const StereoBuffer& audio)
{
    const auto level = measure(audio);
    return "{\"frames\":" + std::to_string(audio.left.size())
        + ",\"peak_dbfs\":" + jsonNumber(decibels(level.peak))
        + ",\"rms_dbfs\":" + jsonNumber(decibels(level.rms))
        + ",\"float_fnv1a64\":\"" + hashAudio(audio) + "\"}";
}

std::string manifestJson(const std::array<StereoBuffer, slugs.size()>& audio,
                         const std::string& label, const std::string& revision,
                         int quality, std::string_view dspFingerprint,
                         std::string_view compiler = YOUKNOW_COMPARISON_COMPILER_ID
                             " " YOUKNOW_COMPARISON_COMPILER_VERSION,
                         std::string_view rendererFingerprint = YOUKNOW_ELECTRONICS_SOURCE_SHA256)
{
    std::ostringstream manifest;
    manifest << "{\n\"protocol\":\"" << protocol << "\",\n"
        << "\"label\":\"" << label << "\",\n"
        << "\"git_revision\":\"" << revision << "\",\n"
        << "\"dsp_source_sha256\":\"" << dspFingerprint << "\",\n"
        << "\"renderer_source_sha256\":\"" << rendererFingerprint << "\",\n"
        << "\"compiler\":\"" << compiler << "\",\n"
        << "\"sample_rate\":" << comparisonSampleRate << ",\n"
        << "\"block_size\":" << comparisonBlockSize << ",\n"
        << "\"quality\":" << quality << ",\n"
        << "\"available_feature_apis\":\"" << capabilities() << "\",\n"
        << "\"feature_api_order\":\"resonance,ota-noise,phones\",\n"
        << "\"latency_compensation\":\"declared integer processing latency only\",\n"
        << "\"discarded_preroll_seconds\":0.25,\n"
        << "\"thermal_start\":\"settled\",\n"
        << "\"noise_seeds\":\"prepare/reset defined fixed seeds\",\n"
        << "\"product_profile\":true,\n"
        << "\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",\n"
        << "\"unit_character\":1,\"aging\":0.5,\"volume\":0.6,\n"
        << "\"velocity\":1,\"velocity_depth\":0,\"polyphony\":6,\n"
        << "\"declared_latency_samples\":\"";
    for (std::size_t index = 0; index + 1 < slugs.size(); ++index)
        manifest << reportedLatency[index] << (index + 2 < slugs.size() ? "," : "");
    manifest << "\",\n\"sections\":{\n";
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        manifest << '\"' << slugs[index] << "\":" << levelJson(audio[index])
                 << (index + 1 < slugs.size() ? ",\n" : "\n");
    }
    manifest << "}\n}\n";
    return manifest.str();
}

void requireUniqueFragment(std::string_view manifest, const std::string& fragment)
{
    const auto start = manifest.find(fragment);
    require(start != std::string_view::npos
        && manifest.find(fragment, start + fragment.size()) == std::string_view::npos,
        "manifest missing or duplicating contract fragment: " + fragment);
}

// Manifests are the tool's deliberately narrow canonical format, not an
// arbitrary JSON ingestion API. Exact fragments avoid accepting one claimed
// quality/identity at the top and another duplicate farther down the file.
std::string manifestString(std::string_view manifest, std::string_view field)
{
    const auto key = "\"" + std::string(field) + "\":";
    requireUniqueFragment(manifest, key);
    const auto begin = manifest.find(key) + key.size();
    require(begin < manifest.size() && manifest[begin] == '\"',
        "manifest string expected: " + std::string(field));
    const auto end = manifest.find('\"', begin + 1);
    require(end != std::string_view::npos, "unterminated manifest string");
    const auto value = manifest.substr(begin + 1, end - begin - 1);
    require(value.find('\\') == std::string_view::npos, "non-canonical escaped manifest string");
    return std::string(value);
}

bool isFingerprint(std::string_view value)
{
    return value.size() == 64
        && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

void validateManifest(const std::string& manifest, const std::string& label,
                      const std::array<StereoBuffer, slugs.size()>& audio)
{
    require(manifestString(manifest, "protocol") == protocol, "wrong render protocol");
    require(manifestString(manifest, "label") == label, "raw render label does not match manifest");
    require(isFingerprint(manifestString(manifest, "dsp_source_sha256")),
        "render requires a 64-digit DSP SHA-256 fingerprint");
    require(isFingerprint(manifestString(manifest, "renderer_source_sha256")),
        "render requires a 64-digit renderer SHA-256 fingerprint");
    require(!manifestString(manifest, "git_revision").empty(), "missing render revision");
    require(manifestString(manifest, "compiler") != "unknown unknown", "missing compiler identity");
    const auto api = manifestString(manifest, "available_feature_apis");
    require(api.size() == 3 && api.find_first_not_of("01") == std::string::npos,
            "invalid feature capability declaration");
    require(manifestString(manifest, "feature_api_order") == "resonance,ota-noise,phones",
            "wrong feature capability order");
    require(manifestString(manifest, "latency_compensation") == "declared integer processing latency only",
            "wrong latency compensation policy");
    const auto latency = manifestString(manifest, "declared_latency_samples");
    std::istringstream latencies(latency);
    std::string element;
    std::size_t latencyCount = 0;
    while (std::getline(latencies, element, ','))
    {
        require(!element.empty() && element.find_first_not_of("0123456789") == std::string::npos,
                "invalid declared latency value");
        require(element.size() <= 4 && std::stoi(element) < 4096, "declared latency out of range");
        ++latencyCount;
    }
    require(latencyCount + 1 == slugs.size(), "declared latency list has wrong size");
    const std::array<std::string, 13> required {
        "\"sample_rate\":48000,", "\"block_size\":128,",
        "\"discarded_preroll_seconds\":0.25,", "\"thermal_start\":\"settled\",",
        "\"noise_seeds\":\"prepare/reset defined fixed seeds\",", "\"product_profile\":true,",
        "\"numerical_modes\":\"PolyZoned/Cubic/Rk4Single\",", "\"unit_character\":1,",
        "\"aging\":0.5,", "\"volume\":0.6,", "\"velocity\":1,",
        "\"velocity_depth\":0,", "\"polyphony\":6,"
    };
    for (const auto& fragment : required)
    {
        requireUniqueFragment(manifest, fragment.substr(0, fragment.find(':') + 1));
        requireUniqueFragment(manifest, fragment);
    }
    const auto qualityKey = std::string("\"quality\":");
    requireUniqueFragment(manifest, qualityKey);
    const auto qualityPosition = manifest.find(qualityKey) + qualityKey.size();
    require(manifest.substr(qualityPosition, 2) == "1,"
        || manifest.substr(qualityPosition, 2) == "4,", "unsupported manifest quality");
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        requireUniqueFragment(manifest, "\"" + std::string(slugs[index]) + "\":");
        requireUniqueFragment(manifest,
            "\"" + std::string(slugs[index]) + "\":" + levelJson(audio[index]));
    }
}

void validatePair(const std::string& beforeManifest, const std::string& afterManifest)
{
    require(manifestString(beforeManifest, "compiler") == manifestString(afterManifest, "compiler"),
        "before/after compiler identity differs");
    require(manifestString(beforeManifest, "renderer_source_sha256")
        == manifestString(afterManifest, "renderer_source_sha256"),
        "before/after renderer source differs; score identity is not established");
    const auto quality = std::string("\"quality\":");
    const auto beforeQuality = beforeManifest.substr(beforeManifest.find(quality) + quality.size(), 2);
    const auto afterQuality = afterManifest.substr(afterManifest.find(quality) + quality.size(), 2);
    require(beforeQuality == afterQuality, "before/after quality differs");
    require(manifestString(beforeManifest, "dsp_source_sha256")
        != manifestString(afterManifest, "dsp_source_sha256"),
        "before/after DSP fingerprints are identical; no source change is being reviewed");
}

std::string readManifest(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "cannot open render manifest " + path.string());
    std::ostringstream contents;
    contents << stream.rdbuf();
    require(stream.good() || stream.eof(), "cannot read render manifest " + path.string());
    return contents.str();
}

void render(const std::filesystem::path& directory, const std::string& label,
            const std::string& revision, int quality)
{
    require(isFingerprint(YOUKNOW_DSP_SOURCE_SHA256),
        "compile the renderer with the CMake DSP source fingerprint");
    require(isFingerprint(YOUKNOW_ELECTRONICS_SOURCE_SHA256),
        "compile the renderer with the CMake renderer source fingerprint");
    const auto output = directory / label;
    require(!std::filesystem::exists(output), "render label already exists; use a new immutable label");
    std::array<StereoBuffer, slugs.size()> audio {
        resonance(quality), otaBreath(quality), chorusSwitch(quality),
        phones(quality), bbdHiss(quality), musical(quality), {}
    };
    constexpr std::size_t montageIndex = slugs.size() - 1;
    for (std::size_t index = 0; index < montageIndex; ++index) {
        append(audio[montageIndex], audio[index]);
        if (index + 1 < montageIndex) silence(audio[montageIndex], .25);
    }
    const auto manifest = manifestJson(audio, label, revision, quality, YOUKNOW_DSP_SOURCE_SHA256);
    validateManifest(manifest, label, audio);
    for (std::size_t index = 0; index < slugs.size(); ++index)
        writeAudio(output / (std::string(slugs[index]) + "-raw.wav"), audio[index]);
    std::string error;
    require(writeText(output / "manifest.json", manifest, error), error);
    const std::string_view score = continuationScore ? R"score(All passages use the product path, 48 kHz /128 frames, fixed reset seeds, settled warm-up and declared-latency compensation. Both builds use the same score and compiler. The baseline is e752eff (DSP identical to 8f7c8a1); final enables the additional supported electronics models. Both builds select the existing 32-ohm Phones route in the two Phones passages. Individual sections include all product changes, so they are revealing passages rather than single-flag causal experiments.

Montage order: resonance 3.2s, voice tail 3.6s, chorus switching 3.6s, bright Phones 3.2s, grounded-volume Phones floor 4s, musical chords 3.65s; .25s silence between sections. Exact frame counts are in the manifest. Normal playing seldom reaches the Phones amplifier's nominal slew limit; its bandwidth change is expected to be subtle. The floor passage exposes noise after the volume control.

The comparison provides immutable raw float32 audio, PCM24 A/B with whole-file stereo RMS matching, signed B-A, and an explicitly boosted residual. RMS matching removes absolute level differences; consult raw metrics for noise-level changes. These are model renders, not new hardware recordings.
)score" : R"score(All passages use the product path, 48 kHz /128 frames, fixed reset seeds, settled warm-up and declared-latency compensation. Both builds use the same score and compiler. The baseline is724aaac; final enables the new supported electronic component models. Phones explicitly selects the new32ohm physical route on final; baseline uses Line. Individual sections include all product changes, so they are revealing passages rather than single-flag causal experiments.

Montage order: resonance3.2s, OTA breath3.6s, chorus switching3.6s, phones3.2s, chorus hiss4s, musical chords3.65s; .25s silence between sections. Exact frame counts are in the manifest.

The comparison provides immutable raw float32 audio, PCM24 A/B with whole-file stereo RMS matching, signed B-A, and an explicitly boosted residual. RMS matching removes absolute level differences; consult raw metrics for noise-level and route-gain changes. These are model renders, not new hardware recordings.
)score";
    require(writeText(output / "score.md", std::string(score), error), error);
    std::cout << "Frozen " << label << ": " << audio[montageIndex].left.size()
              << " montage frames (" << audio[montageIndex].left.size() / 48000.0 << " s)\n";
}

void compare(const std::filesystem::path& directory,
             const std::string& beforeLabel, const std::string& afterLabel)
{
    const auto output = directory / "listen";
    require(!std::filesystem::exists(output), "comparison already exists; keep review evidence frozen");
    const auto beforeManifest = readManifest(directory / beforeLabel / "manifest.json");
    const auto afterManifest = readManifest(directory / afterLabel / "manifest.json");
    std::array<StereoBuffer, slugs.size()> beforeAudio, afterAudio;
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        const auto name = std::string(slugs[index]) + "-raw.wav";
        beforeAudio[index] = readAudio(directory / beforeLabel / name);
        afterAudio[index] = readAudio(directory / afterLabel / name);
    }
    // Finish every identity and sample binding check before writing any audio.
    validateManifest(beforeManifest, beforeLabel, beforeAudio);
    validateManifest(afterManifest, afterLabel, afterAudio);
    validatePair(beforeManifest, afterManifest);
    std::ostringstream manifest;
    manifest << "{\"protocol\":\"" << protocol << "\",\"before\":\"" << beforeLabel
        << "\",\"after\":\"" << afterLabel << "\",\"rms_matching\":\"whole-file stereo\","
        << "\"playback_encoding\":\"PCM24, no dither or extra gain\","
        << "\"matched_metrics_domain\":\"float before PCM24 quantization\","
        << "\"before_dsp_source_sha256\":\"" << manifestString(beforeManifest, "dsp_source_sha256") << "\","
        << "\"after_dsp_source_sha256\":\"" << manifestString(afterManifest, "dsp_source_sha256") << "\","
        << "\"renderer_source_sha256\":\"" << manifestString(beforeManifest, "renderer_source_sha256") << "\","
        << "\"sections\":{\n";
    std::string error;
    for (std::size_t index = 0; index < slugs.size(); ++index)
    {
        const auto slug = std::string(slugs[index]);
        const auto& before = beforeAudio[index];
        const auto& after = afterAudio[index];
        const auto beforeLevel = measure(before);
        const auto afterLevel = measure(after);
        require(beforeLevel.rms > 0 && afterLevel.rms > 0, "cannot match silent audio");
        const double rmsTrim = beforeLevel.rms / afterLevel.rms;
        // Leave enough PCM24 headroom for even an opposite-polarity B-A
        // difference. Both matched takes still sit below the -6dBFS limit.
        const double sharedGain = std::min(listeningTargetPeak, .49)
            / std::max(beforeLevel.peak, afterLevel.peak * rmsTrim);
        const auto matchedBefore = applyGain(before, sharedGain);
        const auto matchedAfter = applyGain(after, sharedGain * rmsTrim);
        StereoBuffer rawDelta, delta;
        require(difference(before, after, rawDelta, error), error);
        require(difference(matchedBefore, matchedAfter, delta, error), error);
        const auto deltaLevel = measure(delta);
        const double audibleGain = deltaLevel.peak > 0
            ? std::min(1000.0, listeningTargetPeak / deltaLevel.peak) : 1.0;
        const auto sectionOutput = output / slug;
        writePlayback(sectionOutput / "before.wav", matchedBefore);
        writePlayback(sectionOutput / "after.wav", matchedAfter);
        writePlayback(sectionOutput / "A.wav", matchedBefore);
        writePlayback(sectionOutput / "B.wav", matchedAfter);
        writeAudio(sectionOutput / "difference-raw.wav", rawDelta);
        writePlayback(sectionOutput / "difference.wav", delta);
        writePlayback(sectionOutput / "difference-audible.wav", applyGain(delta, audibleGain));
        // Convenient direct listen: one complete before, 0.5 s silence,
        // one complete after. Identical section offsets within each half.
        StereoBuffer audition = matchedBefore;
        silence(audition, 0.5);
        append(audition, matchedAfter);
        writePlayback(sectionOutput / "before-then-after.wav", audition);
        const auto key = std::string("A is ") + beforeLabel + " (DSP SHA-256 "
            + manifestString(beforeManifest, "dsp_source_sha256") + ").\n\nB is "
            + afterLabel + " (DSP SHA-256 "
            + manifestString(afterManifest, "dsp_source_sha256") + ").\n\n"
            + "Both are matched on whole-file stereo RMS. B trim: "
            + jsonNumber(decibels(rmsTrim)) + " dB; shared listening gain: "
            + jsonNumber(decibels(sharedGain)) + " dB. The signed difference is B minus A. "
            + "difference-audible.wav adds " + jsonNumber(decibels(audibleGain))
            + " dB to that residual; it is an exaggerated diagnostic.\n";
        require(writeText(sectionOutput / "key.md", key, error), error);
        manifest << '\"' << slug << "\":{\"before_raw\":" << levelJson(before)
            << ",\"after_raw\":" << levelJson(after)
            << ",\"raw_difference\":" << levelJson(rawDelta)
            << ",\"after_rms_trim_db\":" << jsonNumber(decibels(rmsTrim))
            << ",\"shared_gain_db\":" << jsonNumber(decibels(sharedGain))
            << ",\"matched_difference_rms_dbc\":"
            << jsonNumber(decibels(deltaLevel.rms / measure(matchedBefore).rms))
            << ",\"audible_difference_gain_db\":" << jsonNumber(decibels(audibleGain))
            << ",\"matched_before\":" << levelJson(matchedBefore)
            << ",\"matched_after\":" << levelJson(matchedAfter) << '}'
            << (index + 1 < slugs.size() ? ",\n" : "\n");
        std::cout << slug << ": after trim " << decibels(rmsTrim)
            << " dB, matched delta "
            << decibels(deltaLevel.rms / measure(matchedBefore).rms)
            << " dBc, audible delta boost " << decibels(audibleGain) << " dB\n";
    }
    manifest << "}}\n";
    require(writeText(output / "metrics.json", manifest.str(), error), error);
    std::cout << "Native PCM24 review files: " << std::filesystem::absolute(output) << '\n';
}

void selfTest()
{
    std::array<StereoBuffer, slugs.size()> audio;
    for (auto& section : audio)
        section = { { 0.1f, -0.2f, 0.15f }, { -0.05f, 0.2f, -0.1f } };
    const auto makeManifest = [&](std::string_view label, int quality, char fingerprint) {
        return manifestJson(audio, std::string(label), "test-revision", quality,
            std::string(64, fingerprint), "contract-test-compiler 1.0", std::string(64, 'c'));
    };
    const auto before = makeManifest("before", 1, 'a');
    const auto after = makeManifest("after", 1, 'b');
    validateManifest(before, "before", audio);
    validateManifest(after, "after", audio);
    validatePair(before, after);
    int rejected = 0;
    const auto reject = [&](auto&& check, std::string_view name) {
        bool didReject = false;
        try { check(); } catch (const std::runtime_error&) { didReject = true; }
        require(didReject, "self-test failed to reject " + std::string(name));
        ++rejected;
    };
    auto corrupted = audio;
    corrupted[0].left[0] = 0.100001f;
    reject([&] { validateManifest(before, "before", corrupted); }, "corrupt raw audio");
    auto wrongHash = before;
    const auto hashPosition = wrongHash.find(hashAudio(audio[0]));
    wrongHash[hashPosition] = wrongHash[hashPosition] == '0' ? '1' : '0';
    reject([&] { validateManifest(wrongHash, "before", audio); }, "wrong raw hash");
    reject([&] { validateManifest(before, "after", audio); }, "wrong label");
    const auto otherQuality = makeManifest("after", 4, 'b');
    validateManifest(otherQuality, "after", audio);
    reject([&] { validatePair(before, otherQuality); }, "quality mismatch");
    const auto sameSource = makeManifest("after", 1, 'a');
    reject([&] { validatePair(before, sameSource); }, "same DSP fingerprint");
    reject([&] { validateManifest(before + "\"protocol\":\"duplicate\"", "before", audio); },
        "duplicate protocol");
    reject([&] { validateManifest(before + "\"sample_rate\":44100", "before", audio); },
        "duplicate conflicting sample rate");
    auto wrongRate = before;
    wrongRate.replace(wrongRate.find("48000"), 5, "44100");
    reject([&] { validateManifest(wrongRate, "before", audio); }, "wrong sample rate");
    auto wrongCompiler = after;
    const auto compilerPosition = wrongCompiler.find("\"compiler\":\"") + 12;
    wrongCompiler.insert(compilerPosition, "other-");
    reject([&] { validatePair(before, wrongCompiler); }, "compiler mismatch");
    auto wrongRenderer = after;
    wrongRenderer.replace(wrongRenderer.find(std::string(64, 'c')), 64, std::string(64, 'd'));
    validateManifest(wrongRenderer, "after", audio);
    reject([&] { validatePair(before, wrongRenderer); }, "renderer source mismatch");
    auto wrongApis = after;
    const auto apiPosition = wrongApis.find("\"available_feature_apis\":\"") + 26;
    wrongApis[apiPosition] = '2';
    reject([&] { validateManifest(wrongApis, "after", audio); }, "malformed feature capabilities");
    auto wrongLatency = after;
    const auto latencyPosition = wrongLatency.find("\"declared_latency_samples\":\"") + 28;
    wrongLatency.insert(latencyPosition, "-1,");
    reject([&] { validateManifest(wrongLatency, "after", audio); }, "malformed latency declaration");
    const StereoBuffer pcmTest { { -1.f, 0.f, .5f }, { 1.f, -.5f, .25f } };
    const auto pcm = playbackBytes(pcmTest);
    require(pcm.size() == 62 && readU32(pcm.data() + 4) == 54
        && readU16(pcm.data() + 20) == 1 && readU16(pcm.data() + 22) == 2
        && readU32(pcm.data() + 24) == 48000 && readU16(pcm.data() + 32) == 6
        && readU16(pcm.data() + 34) == 24 && readU32(pcm.data() + 40) == 18,
        "PCM24 playback header is invalid");
    // Hand-written signed little-endian codes verify stereo ordering, sign,
    // full scale and fractional values independently of the encoder loop.
    constexpr std::array<std::uint8_t, 18> expectedPcm {
        0x01, 0x00, 0x80, 0xff, 0xff, 0x7f,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xc0,
        0x00, 0x00, 0x40, 0x00, 0x00, 0x20
    };
    require(std::equal(expectedPcm.begin(), expectedPcm.end(), pcm.begin() + 44),
        "PCM24 playback sample encoding is invalid");
    auto clippingPcm = pcmTest;
    clippingPcm.left[0] = 1.000001f;
    reject([&] { playbackBytes(clippingPcm); }, "clipping PCM24 playback");
    auto nonfinitePcm = pcmTest;
    nonfinitePcm.right[0] = std::numeric_limits<float>::quiet_NaN();
    reject([&] { playbackBytes(nonfinitePcm); }, "non-finite PCM24 playback");
    auto unevenPcm = pcmTest;
    unevenPcm.right.pop_back();
    reject([&] { playbackBytes(unevenPcm); }, "unequal PCM24 channels");
    std::cout << "Realism review manifest contract: valid pair passed; " << rejected
              << " invalid comparisons/playback inputs rejected; PCM24 byte oracle passed.\n";
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test")
        {
            selfTest();
            selectContinuationScore();
            selfTest();
            return 0;
        }
        require(argc >= 5, "usage: YouKnowRenderElectronics render dir label git-revision [1|4] | compare dir before-label after-label");
        std::string_view command(argv[1]);
        if (command == "render-next" || command == "compare-next") {
            selectContinuationScore();
            command = command == "render-next" ? "render" : "compare";
        }
        const auto firstLabel = safeLabel(argv[3]);
        if (command == "render")
        {
            require(argc == 5 || argc == 6, "render expects 4 or 5 arguments");
            const auto revision = safeLabel(argv[4]);
            const std::string_view quality = argc == 6 ? argv[5] : "1";
            require(quality == "1" || quality == "4", "quality must be 1 or 4");
            render(argv[2], firstLabel, revision, quality == "4" ? 4 : 1);
        }
        else
        {
            require(command == "compare" && argc == 5, "invalid command or arguments");
            compare(argv[2], firstLabel, safeLabel(argv[4]));
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
