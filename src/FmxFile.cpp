#include "FmxFile.h"

#include <cmath>

namespace fmx
{

//==============================================================================
static double curvePower (const juce::String& curve)
{
    if (curve == "squared") return 2.0;
    if (curve == "cubed")   return 3.0;
    if (curve == "sqrt")    return 0.5;
    if (curve == "cbrt")    return 1.0 / 3.0;
    return 1.0; // linear and anything unknown
}

float ParamSpec::normalise (float v) const
{
    if (curve == "exponential")
    {
        const double mn = minValue <= 0.0f ? 1e-8 : (double) minValue;
        const double mx = juce::jmax ((double) maxValue, mn * 1.000001);
        const double x  = juce::jmax ((double) v, mn);
        return (float) juce::jlimit (0.0, 1.0, (std::log (x) - std::log (mn)) / std::log (mx / mn));
    }

    const double span = (double) maxValue - (double) minValue;
    if (span == 0.0)
        return 0.0f;

    const double t = juce::jlimit (0.0, 1.0, ((double) v - minValue) / span);
    return (float) std::pow (t, 1.0 / curvePower (curve));
}

float ParamSpec::denormalise (float n) const
{
    n = juce::jlimit (0.0f, 1.0f, n);
    double v;

    if (curve == "exponential")
    {
        const double mn = minValue <= 0.0f ? 1e-8 : (double) minValue;
        v = std::exp (std::log (mn) + n * std::log ((double) maxValue / mn));
    }
    else
    {
        v = minValue + std::pow ((double) n, curvePower (curve)) * ((double) maxValue - minValue);
    }

    if (stepped || ! choices.isEmpty())
        v = std::round (v);

    return (float) v;
}

int ParamSpec::displayPrecision() const
{
    // same as the Flow UI: count decimals of `step` (tolerant of float rounding, 0.1f etc.)
    int precision = 0;
    for (double s = step; precision < 6 && std::abs (s - std::round (s)) > 1.0e-4 * juce::jmax (1.0, std::abs (s)); s *= 10.0)
        ++precision;
    return precision;
}

juce::String ParamSpec::format (float v) const
{
    if (! choices.isEmpty())
    {
        const int idx = (int) std::round (v);
        return juce::isPositiveAndBelow (idx, choices.size()) ? choices[idx] : juce::String();
    }

    auto text = juce::String (v, displayPrecision());
    if (displayPrecision() == 0)
        text = juce::String ((int) std::round (v));

    return label.isNotEmpty() ? text + " " + label : text;
}

//==============================================================================
bool decodeGzipBase64 (const juce::String& b64, juce::MemoryBlock& out)
{
    juce::MemoryOutputStream raw;
    if (! juce::Base64::convertFromBase64 (raw, b64.trim()))
        return false;

    juce::MemoryInputStream compressed (raw.getData(), raw.getDataSize(), false);
    juce::GZIPDecompressorInputStream gunzip (&compressed, false,
                                              juce::GZIPDecompressorInputStream::gzipFormat);
    out.reset();
    juce::MemoryOutputStream result (out, false);
    result.writeFromInputStream (gunzip, -1);
    result.flush();
    return out.getSize() > 0;
}

static float toFloat (const juce::var& v, float fallback)
{
    if (v.isDouble() || v.isInt() || v.isInt64() || v.isBool())
        return (float) (double) v;
    if (v.isString())
        return v.toString().getFloatValue();
    return fallback;
}

juce::Result FmxFile::parse (const juce::String& jsonText, FmxFile& out)
{
    juce::var root;
    auto r = juce::JSON::parse (jsonText, root);
    if (r.failed() || ! root.isObject())
        return juce::Result::fail ("Not a valid .fmx file (JSON parse failed)");

    out = {};
    out.formatVersion = root["version"].toString();
    out.name          = root["name"].toString();
    out.type          = root["type"].toString();

    if (! root.hasProperty ("wasm"))
        return juce::Result::fail ("This .fmx file contains no audio engine (wasm)");

    if (! decodeGzipBase64 (root["wasm"].toString(), out.wasm))
        return juce::Result::fail ("Could not decode the audio engine (wasm) in this .fmx file");

    if (root.hasProperty ("html"))
    {
        juce::MemoryBlock htmlBytes;
        if (decodeGzipBase64 (root["html"].toString(), htmlBytes))
            out.html = juce::String::fromUTF8 ((const char*) htmlBytes.getData(), (int) htmlBytes.getSize());
    }

    // metadata is itself a JSON string (but accept an inline object too)
    juce::var meta = root["metadata"];
    if (meta.isString())
        meta = juce::JSON::parse (meta.toString());

    if (auto* params = meta["params"].getArray())
    {
        for (auto& p : *params)
        {
            ParamSpec s;
            s.id           = (int) toFloat (p["id"], 0.0f);
            s.name         = p["name"].toString();
            s.type         = p["type"].toString();
            s.label        = p["label"].toString();
            s.curve        = p.hasProperty ("curve") ? p["curve"].toString() : juce::String ("linear");
            s.minValue     = toFloat (p["min"], 0.0f);
            s.maxValue     = toFloat (p["max"], 1.0f);
            s.defaultValue = toFloat (p["default"], s.minValue);
            s.step         = toFloat (p["step"], 0.0f);
            s.automatable  = p.hasProperty ("automatable") ? (bool) p["automatable"] : true;
            s.stepped      = (bool) p["stepped"];

            if (auto* choices = p["choices"].getArray())
                for (auto& c : *choices)
                    s.choices.add (c.toString());

            if (s.name.isEmpty())
                s.name = "Param " + juce::String (s.id + 1);

            out.params.push_back (s);
        }
    }

    return juce::Result::ok();
}

} // namespace fmx
