// Load only the supplied AU bundle and register its factory in this process.
// Apple documents AudioComponentRegister as process-local; no installation or
// AudioComponentFindNext scan can substitute an older installed instrument.
// https://developer.apple.com/documentation/audiotoolbox/audiocomponentregister(_:_:_:_:)
// This exercises the AU ABI, not Apple's full auval or DAW certification.
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreFoundation/CoreFoundation.h>

#include "PublicParameterOrder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require (bool condition, const char* message)
{
    if (! condition)
        throw std::runtime_error (message);
}

void check (OSStatus status, const char* message)
{
    if (status != noErr)
        throw std::runtime_error (std::string (message) + ": OSStatus="
                                  + std::to_string (status));
}

template <typename Reference>
struct CfLifetime
{
    Reference value = nullptr;
    ~CfLifetime() { if (value != nullptr) CFRelease (value); }
};

struct UnitLifetime
{
    AudioUnit unit = nullptr;
    ~UnitLifetime()
    {
        if (unit != nullptr)
        {
            AudioUnitUninitialize (unit);
            AudioComponentInstanceDispose (unit);
        }
    }
};

template <typename Value>
Value property (AudioUnit unit, AudioUnitPropertyID id,
                AudioUnitScope scope = kAudioUnitScope_Global,
                AudioUnitElement element = 0)
{
    Value value {};
    UInt32 size = sizeof (value);
    check (AudioUnitGetProperty (unit, id, scope, element, &value, &size),
           "AU property read failed");
    require (size == sizeof (value), "wrong AU property size");
    return value;
}

std::string stringFromCf (CFStringRef value)
{
    char text[512] {};
    require (value != nullptr && CFGetTypeID (value) == CFStringGetTypeID()
                 && CFStringGetCString (value, text, sizeof (text), kCFStringEncodingUTF8),
             "invalid AU metadata string");
    return text;
}

UInt32 fourCcFromCf (CFStringRef value)
{
    const auto text = stringFromCf (value);
    require (text.size() == 4, "invalid AU FourCC");
    UInt32 result = 0;
    for (unsigned char byte : text)
        result = (result << 8) | byte;
    return result;
}
} // namespace

int main (int argc, char** argv)
{
    try
    {
        require (argc == 3, "expected local .component bundle path and project version");
        const auto path = std::filesystem::canonical (argv[1]);
        require (path.extension() == ".component", "expected an AU component bundle");
        const auto pathString = path.string();
        CfLifetime<CFURLRef> url { CFURLCreateFromFileSystemRepresentation (
            nullptr, reinterpret_cast<const UInt8*> (pathString.data()),
            static_cast<CFIndex> (pathString.size()), true) };
        require (url.value != nullptr, "AU bundle URL failed");
        const auto bundle = CFBundleCreate (nullptr, url.value);
        require (bundle != nullptr && CFBundleLoadExecutable (bundle),
                 "cannot load the exact AU bundle");
        // Keep this executable loaded until process exit: the local registry
        // retains its factory pointer for the entire process lifetime.
        const auto components = static_cast<CFArrayRef> (
            CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents")));
        require (components != nullptr && CFGetTypeID (components) == CFArrayGetTypeID()
                     && CFArrayGetCount (components) == 1,
                 "expected one AU descriptor");
        const auto metadata = static_cast<CFDictionaryRef> (
            CFArrayGetValueAtIndex (components, 0));
        require (metadata != nullptr && CFGetTypeID (metadata) == CFDictionaryGetTypeID(),
                 "invalid AU descriptor dictionary");
        const auto field = [metadata] (CFStringRef key) {
            return static_cast<CFStringRef> (CFDictionaryGetValue (metadata, key));
        };
        require (stringFromCf (CFBundleGetIdentifier (bundle)) == "cz.protocodus.youknow.au",
                 "wrong AU bundle identifier");
        require (stringFromCf (static_cast<CFStringRef> (
                     CFBundleGetValueForInfoDictionaryKey (
                         bundle, CFSTR ("CFBundleShortVersionString")))) == argv[2],
                 "wrong AU bundle version");
        AudioComponentDescription description {
            fourCcFromCf (field (CFSTR ("type"))),
            fourCcFromCf (field (CFSTR ("subtype"))),
            fourCcFromCf (field (CFSTR ("manufacturer"))),
            kAudioComponentFlag_SandboxSafe, 0 };
        require (description.componentType == 'aumu'
                     && description.componentSubType == 'Yk06'
                     && description.componentManufacturer == 'Ykno',
                 "wrong stable AU identity");
        const auto versionNumber = static_cast<CFNumberRef> (
            CFDictionaryGetValue (metadata, CFSTR ("version")));
        UInt32 version = 0;
        require (versionNumber != nullptr
                     && CFGetTypeID (versionNumber) == CFNumberGetTypeID()
                     && CFNumberGetValue (versionNumber, kCFNumberSInt32Type, &version),
                 "missing AU descriptor version");
        const auto versionString = std::to_string (version >> 16) + "."
            + std::to_string ((version >> 8) & 0xff) + "."
            + std::to_string (version & 0xff);
        require (versionString == argv[2], "wrong AU descriptor version");
        const auto factoryName = field (CFSTR ("factoryFunction"));
        require (stringFromCf (factoryName) == "YouKnowAUFactory",
                 "wrong AU factory export name");
        const auto factory = reinterpret_cast<AudioComponentFactoryFunction> (
            CFBundleGetFunctionPointerForName (bundle, factoryName));
        require (factory != nullptr, "AU factory export missing");
        const auto name = field (CFSTR ("name"));
        require (stringFromCf (name) == "Protocodus: YouKnow", "wrong AU product/vendor name");
        const auto component = AudioComponentRegister (&description, name, version, factory);
        require (component != nullptr, "process-local AU registration failed");
        UnitLifetime instance;
        check (AudioComponentInstanceNew (component, &instance.unit), "AU instance creation");
        const auto unit = instance.unit;
        require (unit != nullptr, "missing AU instance");
        require (property<UInt32> (unit, kAudioUnitProperty_ElementCount,
                                  kAudioUnitScope_Input) == 0,
                 "AU unexpectedly has an input bus");
        require (property<UInt32> (unit, kAudioUnitProperty_ElementCount,
                                  kAudioUnitScope_Output) == 1,
                 "AU output bus count wrong");

        UInt32 listSize = 0;
        Boolean writable = false;
        check (AudioUnitGetPropertyInfo (unit, kAudioUnitProperty_ParameterList,
                                        kAudioUnitScope_Global, 0, &listSize, &writable),
               "AU parameter list size");
        require (listSize == youknow::tests::publicParameterOrder.size()
                                * sizeof (AudioUnitParameterID),
                 "AU public parameter count wrong");
        std::vector<AudioUnitParameterID> ids (listSize / sizeof (AudioUnitParameterID));
        check (AudioUnitGetProperty (unit, kAudioUnitProperty_ParameterList,
                                    kAudioUnitScope_Global, 0, ids.data(), &listSize),
               "AU parameter list");
        require (std::set<AudioUnitParameterID> (ids.begin(), ids.end()).size() == ids.size(),
                 "AU parameter IDs not unique");
        AudioUnitParameterID cutoff = 0;
        bool foundCutoff = false;
        for (auto id : ids)
        {
            const auto info = property<AudioUnitParameterInfo> (
                unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id);
            CfLifetime<CFStringRef> ownedName {
                (info.flags & kAudioUnitParameterFlag_CFNameRelease) != 0
                    ? info.cfNameString : nullptr };
            require (std::isfinite (info.minValue) && std::isfinite (info.maxValue)
                         && info.minValue <= info.maxValue,
                     "invalid AU parameter bounds");
            const auto parameterName = info.cfNameString != nullptr
                ? stringFromCf (info.cfNameString) : std::string (info.name);
            if (parameterName == "VCF Freq")
            {
                cutoff = id;
                foundCutoff = true;
            }
        }
        require (foundCutoff, "AU cutoff parameter missing");
        CfLifetime<CFArrayRef> presets {
            property<CFArrayRef> (unit, kAudioUnitProperty_FactoryPresets) };
        // INIT, the unchanged 128 hardware slots, then 16 original basses/pads.
        require (presets.value != nullptr && CFArrayGetCount (presets.value) == 145,
                 "AU factory program count wrong");

        constexpr UInt32 maximumFrames = 1024;
        check (AudioUnitSetProperty (unit, kAudioUnitProperty_MaximumFramesPerSlice,
                                    kAudioUnitScope_Global, 0,
                                    &maximumFrames, sizeof (maximumFrames)),
               "AU maximum block size");
        const AudioStreamBasicDescription format {
            48000, kAudioFormatLinearPCM,
            kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                | kAudioFormatFlagIsNonInterleaved,
            sizeof (float), 1, sizeof (float), 2, 32, 0 };
        check (AudioUnitSetProperty (unit, kAudioUnitProperty_StreamFormat,
                                    kAudioUnitScope_Output, 0, &format, sizeof (format)),
               "AU stereo float output format");
        check (AudioUnitInitialize (unit), "AU initialize");
        const auto latency = property<Float64> (unit, kAudioUnitProperty_Latency);
        require (std::isfinite (latency) && latency > 0, "AU latency missing");

        CfLifetime<CFPropertyListRef> state {
            property<CFPropertyListRef> (unit, kAudioUnitProperty_ClassInfo) };
        require (state.value != nullptr && CFGetTypeID (state.value) == CFDictionaryGetTypeID(),
                 "AU state dictionary missing");
        std::vector<AudioUnitParameterValue> savedValues (ids.size());
        for (std::size_t index = 0; index < ids.size(); ++index)
            check (AudioUnitGetParameter (unit, ids[index], kAudioUnitScope_Global,
                                         0, &savedValues[index]),
                   "AU saved parameter value");
        AudioUnitParameterValue original = 0;
        check (AudioUnitGetParameter (unit, cutoff, kAudioUnitScope_Global, 0, &original),
               "AU initial cutoff");
        check (AudioUnitSetParameter (unit, cutoff, kAudioUnitScope_Global, 0,
                                     original > 0.5f ? 0.2f : 0.8f, 0),
               "AU parameter edit");
        AudioUnitParameterValue changed = 0;
        check (AudioUnitGetParameter (unit, cutoff, kAudioUnitScope_Global, 0, &changed),
               "AU changed cutoff");
        require (std::abs (changed - original) > 0.1f, "AU parameter edit had no effect");
        check (AudioUnitSetProperty (unit, kAudioUnitProperty_ClassInfo,
                                    kAudioUnitScope_Global, 0, &state.value, sizeof (state.value)),
               "AU state restore");
        for (std::size_t index = 0; index < ids.size(); ++index)
        {
            AudioUnitParameterValue actual = 0;
            check (AudioUnitGetParameter (unit, ids[index], kAudioUnitScope_Global, 0, &actual),
                   "AU restored parameter value");
            require (std::isfinite (actual) && std::abs (actual - savedValues[index]) <= 1.0e-6f,
                     "AU restored state value mismatch");
        }

        std::array<float, maximumFrames> left {}, right {};
        struct StereoBuffers
        {
            UInt32 count;
            AudioBuffer buffers[2];
        } buffers { 2, { { 1, sizeof (left), left.data() },
                         { 1, sizeof (right), right.data() } } };
        static_assert (offsetof (StereoBuffers, buffers) == offsetof (AudioBufferList, mBuffers));
        AudioTimeStamp timestamp {};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        float peak = 0;
        UInt64 framesRendered = 0;
        check (MusicDeviceMIDIEvent (unit, 0x90, 60, 100, 0), "AU MIDI note on");
        for (int block = 0; block < 24; ++block)
        {
            const UInt32 frames = std::array<UInt32, 3> { 64, 256, 1024 }[block % 3];
            if (block == 20)
                check (MusicDeviceMIDIEvent (unit, 0x80, 60, 0, 0), "AU MIDI note off");
            left.fill (0);
            right.fill (0);
            for (auto& buffer : buffers.buffers)
                buffer.mDataByteSize = frames * sizeof (float);
            AudioUnitRenderActionFlags flags = 0;
            check (AudioUnitRender (unit, &flags, &timestamp, 0, frames,
                                    reinterpret_cast<AudioBufferList*> (&buffers)),
                   "AU render");
            for (const auto* channel : { left.data(), right.data() })
                for (UInt32 sample = 0; sample < frames; ++sample)
                {
                    require (std::isfinite (channel[sample]), "AU emitted non-finite samples");
                    peak = std::max (peak, std::abs (channel[sample]));
                }
            timestamp.mSampleTime += frames;
            framesRendered += frames;
        }
        require (peak > 1.0e-5f, "AU MIDI note rendered silence");
        std::cout << "AU descriptor, " << ids.size()
                  << " parameters, 145 presets, state restore and MIDI rendering passed"
                  << " (frames " << framesRendered << ", peak " << peak
                  << ", latency " << latency << " s)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
