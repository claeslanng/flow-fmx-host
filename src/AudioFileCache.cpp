#include "AudioFileCache.h"

namespace fmx
{

AudioFileCache::AudioFileCache()
{
    formats.registerBasicFormats();
}

std::shared_ptr<const DecodedAudio> AudioFileCache::get (const juce::String& path, juce::String* errorOut)
{
    if (path.isEmpty())
        return nullptr;

    if (auto it = cache.find (path); it != cache.end())
        return it->second;

    const juce::File file (path);
    if (! juce::File::isAbsolutePath (path) || ! file.existsAsFile())
    {
        if (errorOut) *errorOut = "Audio file not found";
        return nullptr;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        if (errorOut) *errorOut = "Unsupported or unreadable audio file";
        return nullptr;
    }

    const auto frames = juce::jmin (reader->lengthInSamples, maxFrames);
    const auto channels = (int) juce::jlimit (1u, 2u, reader->numChannels);

    if (frames <= 0)
    {
        if (errorOut) *errorOut = "Audio file is empty";
        return nullptr;
    }

    auto decoded = std::make_shared<DecodedAudio>();
    decoded->path = path;
    decoded->sampleRate = reader->sampleRate > 0 ? reader->sampleRate : 44100.0;
    decoded->buffer.setSize (channels, (int) frames);
    reader->read (&decoded->buffer, 0, (int) frames, 0, true, channels > 1);

    cache[path] = decoded;
    return decoded;
}

juce::var AudioFileCache::inspect (const juce::String& path, int peakBins)
{
    auto* obj = new juce::DynamicObject();
    juce::var result (obj);
    obj->setProperty ("path", path);
    obj->setProperty ("fileName", juce::File (path).getFileName());

    juce::String error;
    auto audio = get (path, &error);

    if (audio == nullptr)
    {
        const bool missing = ! juce::File (path).existsAsFile();
        obj->setProperty ("loaded", false);
        obj->setProperty ("missing", missing);
        obj->setProperty ("error", error);
        return result;
    }

    const int frames = audio->buffer.getNumSamples();
    const int chans  = audio->buffer.getNumChannels();
    peakBins = juce::jlimit (1, 4096, peakBins);

    juce::Array<juce::var> peaks;
    float globalMax = 0.0f;
    std::vector<float> raw ((size_t) peakBins, 0.0f);

    for (int b = 0; b < peakBins; ++b)
    {
        const int start = (int) ((juce::int64) frames * b / peakBins);
        const int end   = juce::jmax (start + 1, (int) ((juce::int64) frames * (b + 1) / peakBins));
        float m = 0.0f;

        for (int c = 0; c < chans; ++c)
            m = juce::jmax (m, audio->buffer.getMagnitude (c, start, juce::jmin (end, frames) - start));

        raw[(size_t) b] = m;
        globalMax = juce::jmax (globalMax, m);
    }

    for (auto v : raw)
        peaks.add (juce::jlimit (0.0f, 1.0f, v));

    obj->setProperty ("loaded", true);
    obj->setProperty ("missing", false);
    obj->setProperty ("numFrames", frames);
    obj->setProperty ("numChannels", chans);
    obj->setProperty ("sampleRate", audio->sampleRate);
    obj->setProperty ("durationSeconds", frames / audio->sampleRate);
    obj->setProperty ("peaks", peaks);
    return result;
}

void AudioFileCache::retainOnly (const juce::StringArray& paths)
{
    for (auto it = cache.begin(); it != cache.end();)
    {
        if (paths.contains (it->first))
            ++it;
        else
            it = cache.erase (it);
    }
}

} // namespace fmx
