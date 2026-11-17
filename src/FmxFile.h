#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace fmx
{

/** One parameter as described in the .fmx "metadata.params" list. */
struct ParamSpec
{
    int id = 0;
    juce::String name;
    juce::String type;      // gain, percent, enum, float, time, frequency, ...
    juce::String label;     // unit label shown after the value ("dB", "%", ...)
    juce::String curve;     // linear, exponential, squared, cubed, sqrt, cbrt
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    float step = 0.0f;
    bool automatable = true;
    bool stepped = false;
    juce::StringArray choices;

    /** Same mapping as the Flow UI runtime (params-runtime.ts). */
    float normalise (float plainValue) const;
    float denormalise (float normalisedValue) const;
    float defaultNormalised() const { return normalise (defaultValue); }

    /** Text shown for a plain value, matching the Flow UI's formatValue(). */
    juce::String format (float plainValue) const;
    int displayPrecision() const;
};

/** A parsed .fmx file: a JSON container holding the WebAssembly DSP, the HTML UI
    and parameter metadata. The wasm and html fields are gzip'ed, base64 encoded. */
struct FmxFile
{
    juce::String formatVersion;
    juce::String name;
    juce::String type;          // "vst_instrument" / "vst_effect"
    juce::MemoryBlock wasm;
    juce::String html;
    std::vector<ParamSpec> params;

    bool isInstrument() const { return type.containsIgnoreCase ("instrument"); }
    bool isEffect() const     { return type.containsIgnoreCase ("effect"); }

    /** Parses .fmx JSON text. Returns an error message on failure. */
    static juce::Result parse (const juce::String& jsonText, FmxFile& out);
};

/** base64 -> gunzip helper. */
bool decodeGzipBase64 (const juce::String& b64, juce::MemoryBlock& out);

} // namespace fmx
