// End-to-end test of the plugin's processor (no GUI): loads an .fmx, points it at a
// generated sample, plays notes, checks parameters, and round-trips the saved state.
//
//   fmx_plugin_test <file.fmx>
//
// Exit code 0 = all checks passed. Runs in CI on Windows to verify the real build.

#include "PluginProcessor.h"

#include <iostream>

static int failures = 0;

static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  [ok]   " : "  [FAIL] ") << what << "\n";
    if (! ok) ++failures;
}

static float renderBlocks (juce::AudioProcessor& p, int numBlocks, juce::MidiBuffer firstBlockMidi = {})
{
    juce::AudioBuffer<float> buf (2, 512);
    float peak = 0.0f;

    for (int b = 0; b < numBlocks; ++b)
    {
        juce::MidiBuffer midi;
        if (b == 0) midi = firstBlockMidi;
        buf.clear();
        p.processBlock (buf, midi);
        peak = juce::jmax (peak, buf.getMagnitude (0, 0, 512), buf.getMagnitude (1, 0, 512));
    }

    return peak;
}

int main (int argc, char* argv[])
{
    if (argc < 2) { std::cerr << "usage: fmx_plugin_test <file.fmx>\n"; return 2; }

    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto fmxFile = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);

    // a test sample: 1 s, 220 Hz saw at -6 dB
    auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("fmx_test_saw.wav");
    {
        juce::AudioBuffer<float> b (1, 48000);
        for (int i = 0; i < 48000; ++i)
            b.setSample (0, i, (float) (std::fmod (i * 220.0 / 48000.0, 1.0) * 2.0 - 1.0) * 0.5f);
        wav.deleteFile();
        juce::WavAudioFormat fmt;
        auto os = wav.createOutputStream();
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), 48000, 1, 16, {}, 0));
        os.release();
        w->writeFromAudioSampleBuffer (b, 0, 48000);
    }

    auto customJson = "{\"samplePath\":" + juce::JSON::toString (wav.getFullPathName()) + "}";

    std::unique_ptr<FlowFmxProcessor> p (static_cast<FlowFmxProcessor*> (createPluginFilter()));
    p->setPlayConfigDetails (0, 2, 48000.0, 512);
    p->prepareToPlay (48000.0, 512);

    std::cout << "loading " << fmxFile.getFullPathName() << "\n";
    auto r = p->loadFmxFile (fmxFile);
    check (r.wasOk(), "load .fmx: " + (r.wasOk() ? p->getStatusText() : r.getErrorMessage()));
    if (r.failed()) return 1;

    int active = 0;
    for (int i = 0; i < FlowFmxProcessor::numSlots; ++i)
        if (p->getSlot (i)->isActive())
        {
            ++active;
            std::cout << "         param " << i << ": " << p->getSlot (i)->getName (64) << " = "
                      << p->getSlot (i)->getText (p->getSlot (i)->getValue(), 64) << "\n";
        }
    check (active == (int) p->getFmx().params.size(), "parameters mapped: " + juce::String (active));

    p->setCustomStateJson (customJson);

#if FMX_IS_SYNTH
    juce::MidiBuffer on;  on.addEvent (juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100), 10);
    const float sustainPeak = renderBlocks (*p, 50, on);
    check (sustainPeak > 0.1f, "note on produces sound (peak " + juce::String (sustainPeak, 3) + ")");

    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, 57), 0);
    renderBlocks (*p, 150, off); // ~1.6 s; the default release is a 200 ms exponential decay
    const float tail = renderBlocks (*p, 5);
    check (tail < 0.01f, "note off releases (peak " + juce::String (tail, 4) + ")");

    // Volume (slot 0) to minimum -> much quieter
    auto* vol = p->getSlot (0);
    const float volDefault = vol->getValue();
    vol->setValue (0.0f);
    const float quiet = renderBlocks (*p, 20, on);
    check (quiet < sustainPeak * 0.05f, "volume parameter reaches the DSP (peak " + juce::String (quiet, 4) + ")");
    renderBlocks (*p, 60, off);
    vol->setValue (volDefault);

    // CPU: render 8 voices for 5 s and time it
    juce::MidiBuffer chord;
    for (int n : { 48, 52, 55, 60, 64, 67, 72, 76 })
        chord.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 90), 0);
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    renderBlocks (*p, (int) (5.0 * 48000 / 512), chord);
    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    std::cout << "         8 voices: 5 s of audio rendered in " << juce::String (ms, 1) << " ms ("
              << juce::String (5000.0 / ms, 1) << "x realtime)\n";
    juce::MidiBuffer allOff; allOff.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    renderBlocks (*p, 60, allOff);
#endif

    // State round trip into a fresh instance
    juce::MemoryBlock state;
    p->getStateInformation (state);
    check (state.getSize() > 1000, "state saved (" + juce::String ((int) state.getSize()) + " bytes)");

    std::unique_ptr<FlowFmxProcessor> p2 (static_cast<FlowFmxProcessor*> (createPluginFilter()));
    p2->setPlayConfigDetails (0, 2, 48000.0, 512);
    p2->prepareToPlay (48000.0, 512);
    p2->setStateInformation (state.getData(), (int) state.getSize());
    check (p2->hasFmx(), "state restores the .fmx (" + p2->getStatusText() + ")");
    check (p2->getCustomState()["samplePath"].toString() == wav.getFullPathName(), "state restores the sample path");

#if FMX_IS_SYNTH
    const float restoredPeak = renderBlocks (*p2, 30, on);
    check (restoredPeak > 0.1f, "restored instance plays (peak " + juce::String (restoredPeak, 3) + ")");
#endif

    p.reset();
    p2.reset();
    wav.deleteFile();

    std::cout << (failures == 0 ? "ALL CHECKS PASSED\n" : "SOME CHECKS FAILED\n");
    return failures == 0 ? 0 : 1;
}
