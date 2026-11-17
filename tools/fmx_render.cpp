// Headless test tool: loads an .fmx file, plays a few notes through its WebAssembly DSP
// and writes the result to a WAV file. Used to verify the engine without a DAW.
//
//   fmx_render <file.fmx> <out.wav> [sample.wav]
//
// If no sample is given, a generated 220 Hz saw wave is supplied to sampler-style plugins.

#include "AudioFileCache.h"
#include "FmxFile.h"
#include "WasmDsp.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <iostream>

int main (int argc, char* argv[])
{
    if (argc < 3)
    {
        std::cerr << "usage: fmx_render <file.fmx> <out.wav> [sample.wav]\n";
        return 2;
    }

    const juce::File fmxPath (juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]));
    const juce::File outPath (juce::File::getCurrentWorkingDirectory().getChildFile (argv[2]));
    const juce::String samplePath = argc > 3 ? juce::File::getCurrentWorkingDirectory().getChildFile (argv[3]).getFullPathName()
                                             : juce::String ("/generated/saw.wav");
    const double sr = 48000.0;

    fmx::FmxFile file;
    auto r = fmx::FmxFile::parse (fmxPath.loadFileAsString(), file);
    if (r.failed()) { std::cerr << r.getErrorMessage() << "\n"; return 1; }

    std::cout << "name=" << file.name << " type=" << file.type << " version=" << file.formatVersion
              << " wasm=" << file.wasm.getSize() << "B html=" << file.html.length() << " chars params="
              << file.params.size() << "\n";

    juce::String err;
    auto dsp = fmx::WasmDsp::create (file.wasm, err);
    if (dsp == nullptr) { std::cerr << "create failed: " << err << "\n"; return 1; }

    // audio provider: real file via cache, or a generated saw
    fmx::AudioFileCache cache;
    auto saw = std::make_shared<fmx::DecodedAudio>();
    saw->sampleRate = sr;
    saw->buffer.setSize (1, (int) sr);
    for (int i = 0; i < (int) sr; ++i)
        saw->buffer.setSample (0, i, (float) (std::fmod (i * 220.0 / sr, 1.0) * 2.0 - 1.0));

    dsp->setAudioProvider ([&] (const juce::String& p) -> std::shared_ptr<const fmx::DecodedAudio>
    {
        std::cout << "  dsp opened audio file: " << p << "\n";
        if (p == "/generated/saw.wav") return saw;
        return cache.get (p);
    });
    dsp->setStateStrings ({ { "samplePath", samplePath.toStdString() } });

    for (auto& p : file.params)
        dsp->setParam (p.id, p.defaultValue);

    if (! dsp->init (sr) || ! dsp->stateChanged())
    {
        std::cerr << "init failed: " << dsp->getError() << "\n";
        return 1;
    }

    std::cout << "abi=" << dsp->getAbiVersion() << " getNumParams=" << dsp->getNumParams() << "\n";
    for (auto& b : dsp->getExposedBuffers())
        std::cout << "  exposed buffer '" << b.name << "' count=" << b.count << "\n";

    // render: C-E-G chord for 1.5 s, then 1 s release; also time the DSP
    const int block = 512, total = (int) (sr * 2.5);
    juce::AudioBuffer<float> out (2, total);
    out.clear();

    auto t0 = juce::Time::getMillisecondCounterHiRes();
    bool released = false;
    for (int pos = 0; pos < total; pos += block)
    {
        if (pos == 0) { dsp->noteOn (60, 100); dsp->noteOn (64, 100); dsp->noteOn (67, 100); }
        if (! released && pos >= (int) (sr * 1.5))
        {
            released = true;
            dsp->noteOff (60); dsp->noteOff (64); dsp->noteOff (67);
        }
        const int n = juce::jmin (block, total - pos);
        dsp->process (nullptr, nullptr, out.getWritePointer (0, pos), out.getWritePointer (1, pos), n);
    }
    auto ms = juce::Time::getMillisecondCounterHiRes() - t0;

    if (dsp->hasFailed()) { std::cerr << "dsp error: " << dsp->getError() << "\n"; return 1; }

    std::cout << "rendered " << total / sr << " s in " << ms << " ms ("
              << juce::String ((total / sr * 1000.0) / ms, 1) << "x realtime)\n";
    for (int s = 0; s < total; s += (int) (sr * 0.25))
        std::cout << "  t=" << juce::String (s / sr, 2) << "s peak="
                  << juce::String (out.getMagnitude (0, s, juce::jmin ((int) (sr * 0.25), total - s)), 3) << "\n";

    juce::WavAudioFormat wav;
    outPath.deleteFile();
    std::unique_ptr<juce::OutputStream> os (outPath.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 16, {}, 0));
    if (w == nullptr) { std::cerr << "cannot write " << outPath.getFullPathName() << "\n"; return 1; }
    os.release();
    w->writeFromAudioSampleBuffer (out, 0, total);
    std::cout << "wrote " << outPath.getFullPathName() << "\n";
    return 0;
}
