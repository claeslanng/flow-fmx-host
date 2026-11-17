#pragma once

#include "AudioFileCache.h"
#include "FmxFile.h"
#include "WasmDsp.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <bitset>

#ifndef FMX_IS_SYNTH
 #define FMX_IS_SYNTH 1
#endif

//==============================================================================
/** A host-visible parameter slot. The plugin always exposes the same number of slots
    (VST3 hosts don't like parameter lists that change); loading an .fmx file assigns
    names/ranges to the first N of them. The stored value is normalised 0..1, exactly
    like the Flow UI's own normalised values. */
class FmxParameter : public juce::AudioProcessorParameterWithID
{
public:
    explicit FmxParameter (int slotIndex)
        : AudioProcessorParameterWithID (juce::ParameterID { "p" + juce::String (slotIndex), 1 },
                                         "Param " + juce::String (slotIndex + 1)),
          slot (slotIndex) {}

    void setSpec (const fmx::ParamSpec* spec);   // nullptr = unused slot
    bool isActive() const;
    fmx::ParamSpec getSpec() const;

    float getValue() const override                    { return value.load(); }
    void  setValue (float v) override                  { value.store (juce::jlimit (0.0f, 1.0f, v)); }
    float getDefaultValue() const override;
    juce::String getName (int maximumStringLength) const override;
    juce::String getLabel() const override             { return {}; }
    juce::String getText (float normalisedValue, int maximumStringLength) const override;
    float getValueForText (const juce::String& text) const override;
    bool isAutomatable() const override                { return true; }

    const int slot;

private:
    std::atomic<float> value { 0.0f };
    mutable juce::CriticalSection specLock;
    fmx::ParamSpec spec;
    bool active = false;
};

//==============================================================================
class FlowFmxProcessor : public juce::AudioProcessor,
                         public juce::ChangeBroadcaster
{
public:
    static constexpr int numSlots = fmx::WasmDsp::maxParams;

    FlowFmxProcessor();
    ~FlowFmxProcessor() override;

    //== AudioProcessor ============================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                      { return true; }

    const juce::String getName() const override          { return JucePlugin_Name; }
    bool acceptsMidi() const override                    { return FMX_IS_SYNTH != 0; }
    bool producesMidi() const override                   { return false; }
    bool isMidiEffect() const override                   { return false; }
    double getTailLengthSeconds() const override         { return FMX_IS_SYNTH ? 0.0 : 1.0; }

    int getNumPrograms() override                        { return 1; }
    int getCurrentProgram() override                     { return 0; }
    void setCurrentProgram (int) override                {}
    const juce::String getProgramName (int) override     { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //== .fmx handling (message thread) ===========================================
    juce::Result loadFmxFile (const juce::File& file);
    juce::Result loadFmxText (const juce::String& json, const juce::String& path, bool resetParams);

    /** Unloads the current .fmx and returns to the empty state. */
    void unloadFmx();

    bool hasFmx() const                                  { return fmxLoaded; }
    const fmx::FmxFile& getFmx() const                   { return fmx; }
    juce::String getFmxPath() const                      { return fmxPath; }
    juce::String getStatusText() const                   { return statusText; }

    FmxParameter* getSlot (int i)                        { return juce::isPositiveAndBelow (i, numSlots) ? slots[(size_t) i] : nullptr; }

    //== Flow "custom state" (JSON object the UI persists, e.g. { samplePath }) ====
    juce::var getCustomState() const                     { return customState; }
    void setCustomStateJson (const juce::String& json);

    juce::var inspectAudioFile (const juce::String& path, int peakBins);
    juce::String getAudioWildcard() const                { return audioCache.getWildcard(); }

    //== data for the UI bridge ====================================================
    struct TelemetrySnapshot
    {
        static constexpr int maxValues = 512;
        std::string name;
        std::atomic<int> count { 0 };
        std::array<std::atomic<float>, maxValues> values {};
    };

    static constexpr int maxTelemetry = 4;
    std::array<TelemetrySnapshot, maxTelemetry> telemetry;
    std::atomic<int> numTelemetry { 0 };
    std::atomic<uint32_t> telemetryCounter { 0 };

    struct Transport
    {
        std::atomic<double> tempo { 120.0 }, ppqPos { 0.0 }, samplePos { 0.0 }, lastBarPpq { -1.0 },
                            cycleStart { -1.0 }, cycleEnd { -1.0 };
        std::atomic<int> sigNum { 4 }, sigDen { 4 };
        std::atomic<bool> playing { false }, looping { false };
    } transport;

    /** Window size chosen by the user for the current .fmx ({0,0} = not set yet). */
    juce::Point<int> editorSize;

    juce::var getHostInfo() const;
    juce::var getTransportInfo() const;

private:
    static BusesProperties makeBuses();

    struct Engine
    {
        std::unique_ptr<fmx::WasmDsp> dsp;
        std::vector<fmx::ParamSpec> specs;
    };

    std::map<std::string, std::string> customStateStrings() const;
    void applyCustomStateToDsp();
    void renderRange (fmx::WasmDsp&, juce::AudioBuffer<float>&, int start, int end);
    void handleMidi (fmx::WasmDsp&, const juce::MidiMessage&);
    void captureTelemetry (fmx::WasmDsp&);
    void updateTransport();

    std::array<FmxParameter*, numSlots> slots {};

    // The audio thread only ever try-locks this, so the message thread can swap the engine.
    juce::SpinLock engineLock;
    Engine engine;

    fmx::FmxFile fmx;
    juce::String fmxText, fmxPath, statusText;
    bool fmxLoaded = false;

    juce::var customState;
    fmx::AudioFileCache audioCache;
    juce::CriticalSection audioCacheLock;

    double currentSampleRate = 44100.0;
    int currentBlockSize = 512;
    std::bitset<128> heldNotes;
    juce::AudioBuffer<float> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlowFmxProcessor)
};
