#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>
#include <memory>

namespace fmx
{

struct DecodedAudio
{
    juce::String path;
    juce::AudioBuffer<float> buffer;
    double sampleRate = 44100.0;
};

/** Decodes and keeps audio files that the DSP or UI ask for. Message thread only. */
class AudioFileCache
{
public:
    AudioFileCache();

    /** Returns the decoded file, decoding it on first use. nullptr if missing/unreadable. */
    std::shared_ptr<const DecodedAudio> get (const juce::String& path, juce::String* errorOut = nullptr);

    /** Builds the payload the Flow UI expects for window.__vstOnAudioFileInspected(). */
    juce::var inspect (const juce::String& path, int peakBins);

    /** File-chooser wildcard for all readable formats. */
    juce::String getWildcard() const { return formats.getWildcardForAllFormats(); }

    /** Drop files that are not referenced by any of the given paths. */
    void retainOnly (const juce::StringArray& paths);

private:
    juce::AudioFormatManager formats;
    std::map<juce::String, std::shared_ptr<const DecodedAudio>> cache;

    // safety cap so a mistakenly chosen hour-long file can't eat all memory
    static constexpr juce::int64 maxFrames = 48000LL * 60 * 10; // 10 minutes @ 48k
};

} // namespace fmx
