#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// FmxParameter
//==============================================================================
void FmxParameter::setSpec (const fmx::ParamSpec* s)
{
    const juce::ScopedLock sl (specLock);
    active = s != nullptr;
    spec = s != nullptr ? *s : fmx::ParamSpec();
}

bool FmxParameter::isActive() const
{
    const juce::ScopedLock sl (specLock);
    return active;
}

fmx::ParamSpec FmxParameter::getSpec() const
{
    const juce::ScopedLock sl (specLock);
    return spec;
}

float FmxParameter::getDefaultValue() const
{
    const juce::ScopedLock sl (specLock);
    return active ? spec.defaultNormalised() : 0.0f;
}

juce::String FmxParameter::getName (int maximumStringLength) const
{
    const juce::ScopedLock sl (specLock);
    auto name = active ? spec.name : "(unused " + juce::String (slot + 1) + ")";
    return maximumStringLength > 0 ? name.substring (0, maximumStringLength) : name;
}

juce::String FmxParameter::getText (float normalisedValue, int maximumStringLength) const
{
    const juce::ScopedLock sl (specLock);
    auto text = active ? spec.format (spec.denormalise (normalisedValue))
                       : juce::String (normalisedValue, 3);
    return maximumStringLength > 0 ? text.substring (0, maximumStringLength) : text;
}

float FmxParameter::getValueForText (const juce::String& text) const
{
    const juce::ScopedLock sl (specLock);
    if (! active)
        return text.getFloatValue();

    const int choice = spec.choices.indexOf (text.trim(), true);
    if (choice >= 0)
        return spec.normalise ((float) choice);

    return spec.normalise (text.retainCharacters ("0123456789.-+eE").getFloatValue());
}

//==============================================================================
// FlowFmxProcessor
//==============================================================================
juce::AudioProcessor::BusesProperties FlowFmxProcessor::makeBuses()
{
   #if FMX_IS_SYNTH
    return BusesProperties()
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #else
    return BusesProperties()
        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #endif
}

FlowFmxProcessor::FlowFmxProcessor()
    : AudioProcessor (makeBuses())
{
    for (int i = 0; i < numSlots; ++i)
    {
        auto* p = new FmxParameter (i);
        slots[(size_t) i] = p;
        addParameter (p);
    }

    customState = juce::var (new juce::DynamicObject());
    statusText = "No .fmx loaded";
}

FlowFmxProcessor::~FlowFmxProcessor()
{
    const juce::SpinLock::ScopedLockType sl (engineLock);
    engine = {};
}

bool FlowFmxProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;

   #if ! FMX_IS_SYNTH
    if (layouts.getMainInputChannelSet() != out)
        return false;
   #endif

    return true;
}

//==============================================================================
std::map<std::string, std::string> FlowFmxProcessor::customStateStrings() const
{
    std::map<std::string, std::string> result;

    if (auto* obj = customState.getDynamicObject())
    {
        for (auto& prop : obj->getProperties())
        {
            const auto& v = prop.value;
            const auto s = v.isString() ? v.toString() : juce::JSON::toString (v, true);
            result[prop.name.toString().toStdString()] = s.toStdString();
        }
    }

    return result;
}

void FlowFmxProcessor::applyCustomStateToDsp()
{
    // Decode any audio files referenced by the state *before* taking the engine lock,
    // so the audio thread is only blocked for the short copy into the DSP's memory.
    juce::StringArray referenced;
    for (auto& [key, value] : customStateStrings())
    {
        juce::ignoreUnused (key);
        const auto path = juce::String::fromUTF8 (value.c_str());
        if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
        {
            referenced.add (path);
            const juce::ScopedLock cl (audioCacheLock);
            audioCache.get (path);
        }
    }

    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        if (engine.dsp != nullptr)
        {
            engine.dsp->setStateStrings (customStateStrings());
            engine.dsp->stateChanged();
        }
    }

    const juce::ScopedLock cl (audioCacheLock);
    audioCache.retainOnly (referenced);
}

void FlowFmxProcessor::setCustomStateJson (const juce::String& json)
{
    auto parsed = juce::JSON::parse (json);
    customState = parsed.isObject() ? parsed : juce::var (new juce::DynamicObject());
    applyCustomStateToDsp();
}

juce::var FlowFmxProcessor::inspectAudioFile (const juce::String& path, int peakBins)
{
    const juce::ScopedLock cl (audioCacheLock);
    return audioCache.inspect (path, peakBins);
}

//==============================================================================
juce::Result FlowFmxProcessor::loadFmxFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("File not found: " + file.getFullPathName());

    return loadFmxText (file.loadFileAsString(), file.getFullPathName(), true);
}

juce::Result FlowFmxProcessor::loadFmxText (const juce::String& json, const juce::String& path, bool resetParams)
{
    fmx::FmxFile parsed;
    auto r = fmx::FmxFile::parse (json, parsed);
    if (r.failed())
        return r;

    juce::String err;
    auto dsp = fmx::WasmDsp::create (parsed.wasm, err);
    if (dsp == nullptr)
        return juce::Result::fail ("Could not start the plugin's audio engine: " + err);

    if (dsp->getAbiVersion() > 1)
        DBG ("fmx: engine ABI version " << dsp->getAbiVersion() << " is newer than this host knows");

    // If the metadata has no parameter list, fall back to generic parameters.
    auto specs = parsed.params;
    if (specs.empty())
    {
        const int n = juce::jlimit (0, numSlots, dsp->getNumParams());
        for (int i = 0; i < n; ++i)
        {
            fmx::ParamSpec s;
            s.id = i;
            s.name = "Param " + juce::String (i + 1);
            specs.push_back (s);
        }
    }

    specs.erase (std::remove_if (specs.begin(), specs.end(),
                                 [] (const fmx::ParamSpec& s) { return ! juce::isPositiveAndBelow (s.id, numSlots); }),
                 specs.end());

    // The DSP reads audio files through the shared cache (decoding on demand).
    dsp->setAudioProvider ([this] (const juce::String& p)
    {
        const juce::ScopedLock cl (audioCacheLock);
        return audioCache.get (p);
    });

    // Assign parameter slots
    for (auto* slot : slots)
        slot->setSpec (nullptr);

    for (auto& s : specs)
    {
        auto* slot = slots[(size_t) s.id];
        slot->setSpec (&s);
        if (resetParams)
            slot->setValueNotifyingHost (s.defaultNormalised());
    }

    if (resetParams)
    {
        customState = juce::var (new juce::DynamicObject());
        editorSize = {};
    }

    // Initialise the new engine before it goes live
    for (auto& s : specs)
        dsp->setParam (s.id, s.denormalise (slots[(size_t) s.id]->getValue()));

    dsp->setStateStrings (customStateStrings());

    if (! dsp->init (currentSampleRate) || ! dsp->stateChanged())
        return juce::Result::fail ("The plugin's audio engine failed to start: " + dsp->getError());

    {
        const juce::SpinLock::ScopedLockType sl (engineLock);
        engine.dsp = std::move (dsp);
        engine.specs = specs;
        heldNotes.reset();

        int n = 0;
        for (auto& b : engine.dsp->getExposedBuffers())
        {
            if (n >= maxTelemetry) break;
            telemetry[(size_t) n].name = b.name;
            telemetry[(size_t) n].count = 0;
            ++n;
        }
        numTelemetry = n;
    }

    fmx = std::move (parsed);
    fmxText = json;
    fmxPath = path;
    fmxLoaded = true;

    statusText = fmx.name.isNotEmpty() ? fmx.name : juce::File (path).getFileNameWithoutExtension();
   #if FMX_IS_SYNTH
    if (fmx.isEffect())
        statusText << "  (this is an effect: load it in \"Flow FMX Effect\" instead)";
   #else
    if (fmx.isInstrument())
        statusText << "  (this is an instrument: load it in \"Flow FMX Instrument\" instead)";
   #endif

    updateHostDisplay (ChangeDetails().withParameterInfoChanged (true));
    sendChangeMessage();
    return juce::Result::ok();
}

//==============================================================================
void FlowFmxProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentBlockSize = samplesPerBlock;
    scratch.setSize (2, juce::jmax (samplesPerBlock, 512));

    if (sampleRate == currentSampleRate && engine.dsp != nullptr)
        return;

    currentSampleRate = sampleRate;

    const juce::SpinLock::ScopedLockType sl (engineLock);
    if (engine.dsp != nullptr && ! engine.dsp->hasFailed())
    {
        engine.dsp->init (sampleRate);
        engine.dsp->stateChanged();
        heldNotes.reset();
    }
}

void FlowFmxProcessor::updateTransport()
{
    auto* ph = getPlayHead();
    if (ph == nullptr)
        return;

    if (auto pos = ph->getPosition())
    {
        if (auto bpm = pos->getBpm())                         transport.tempo = *bpm;
        if (auto ppq = pos->getPpqPosition())                 transport.ppqPos = *ppq;
        if (auto smp = pos->getTimeInSamples())               transport.samplePos = (double) *smp;
        if (auto bar = pos->getPpqPositionOfLastBarStart())   transport.lastBarPpq = *bar;
        if (auto sig = pos->getTimeSignature())               { transport.sigNum = sig->numerator; transport.sigDen = sig->denominator; }
        if (auto loop = pos->getLoopPoints())                 { transport.cycleStart = loop->ppqStart; transport.cycleEnd = loop->ppqEnd; }
        transport.playing = pos->getIsPlaying();
        transport.looping = pos->getIsLooping();
    }
}

void FlowFmxProcessor::handleMidi (fmx::WasmDsp& dsp, const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        const int note = m.getNoteNumber();
        dsp.noteOn (note, juce::jlimit (1, 127, (int) m.getVelocity()));
        heldNotes.set ((size_t) note);
    }
    else if (m.isNoteOff())
    {
        const int note = m.getNoteNumber();
        dsp.noteOff (note);
        heldNotes.reset ((size_t) note);
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (int n = 0; n < 128; ++n)
            if (heldNotes.test ((size_t) n))
                dsp.noteOff (n);
        heldNotes.reset();
    }
}

void FlowFmxProcessor::renderRange (fmx::WasmDsp& dsp, juce::AudioBuffer<float>& buffer, int start, int end)
{
    const int n = end - start;
    if (n <= 0)
        return;

    const int numIn  = getTotalNumInputChannels();
    const int numOut = buffer.getNumChannels();

    const float* inL = nullptr;
    const float* inR = nullptr;
   #if ! FMX_IS_SYNTH
    if (numIn > 0) inL = buffer.getReadPointer (0, start);
    if (numIn > 1) inR = buffer.getReadPointer (1, start);
   #else
    juce::ignoreUnused (numIn);
   #endif

    float* outL = numOut > 0 ? buffer.getWritePointer (0, start) : scratch.getWritePointer (0);
    float* outR = numOut > 1 ? buffer.getWritePointer (1, start) : scratch.getWritePointer (1);

    if (numOut < 2 && scratch.getNumSamples() < n)
        return; // can't happen after prepareToPlay; be defensive on the audio thread

    dsp.process (inL, inR, outL, outR, n);

    if (numOut == 1) // mono output: mix both channels
    {
        juce::FloatVectorOperations::add (outL, outR, n);
        juce::FloatVectorOperations::multiply (outL, 0.5f, n);
    }
}

void FlowFmxProcessor::captureTelemetry (fmx::WasmDsp& dsp)
{
    float tmp[TelemetrySnapshot::maxValues];
    const auto& exposed = dsp.getExposedBuffers();
    const int n = juce::jmin ((int) exposed.size(), numTelemetry.load());

    for (int i = 0; i < n; ++i)
    {
        const int count = dsp.readExposed (exposed[(size_t) i], tmp, TelemetrySnapshot::maxValues);
        auto& snap = telemetry[(size_t) i];
        for (int k = 0; k < count; ++k)
            snap.values[(size_t) k].store (tmp[k], std::memory_order_relaxed);
        snap.count.store (count, std::memory_order_release);
    }

    telemetryCounter.fetch_add (1, std::memory_order_relaxed);
}

void FlowFmxProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    updateTransport();

    const int numSamples = buffer.getNumSamples();

    // clear outputs that have no input
    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const juce::SpinLock::ScopedTryLockType tl (engineLock);
    if (! tl.isLocked() || engine.dsp == nullptr || engine.dsp->hasFailed())
    {
       #if FMX_IS_SYNTH
        buffer.clear();
       #endif
        return; // effects pass audio through untouched while no engine is running
    }

    auto& dsp = *engine.dsp;

    for (auto& s : engine.specs)
        dsp.setParam (s.id, s.denormalise (slots[(size_t) s.id]->getValue()));

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        renderRange (dsp, buffer, pos, at);
        pos = at;
        handleMidi (dsp, meta.getMessage());
    }

    renderRange (dsp, buffer, pos, numSamples);
    captureTelemetry (dsp);

    if (dsp.hasFailed())
        buffer.clear();
}

//==============================================================================
juce::var FlowFmxProcessor::getHostInfo() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("host", juce::PluginHostType().getHostDescription());
    o->setProperty ("api", "VST3");
    o->setProperty ("maxBlockSize", currentBlockSize);
    o->setProperty ("sampleRate", currentSampleRate);
    o->setProperty ("isOffline", isNonRealtime());
    return juce::var (o);
}

juce::var FlowFmxProcessor::getTransportInfo() const
{
    auto* o = new juce::DynamicObject();
    const double tempo = transport.tempo.load();
    o->setProperty ("tempo", tempo);
    o->setProperty ("ppqPos", transport.ppqPos.load());
    o->setProperty ("samplePos", transport.samplePos.load());
    o->setProperty ("samplesPerBeat", tempo > 0 ? currentSampleRate * 60.0 / tempo : 22050.0);
    o->setProperty ("isPlaying", transport.playing.load());
    o->setProperty ("timeSigNumerator", transport.sigNum.load());
    o->setProperty ("timeSigDenominator", transport.sigDen.load());
    o->setProperty ("isLooping", transport.looping.load());
    o->setProperty ("cycleStart", transport.cycleStart.load());
    o->setProperty ("cycleEnd", transport.cycleEnd.load());
    o->setProperty ("lastBarBeat", transport.lastBarPpq.load());
    o->setProperty ("isOffline", isNonRealtime());
    return juce::var (o);
}

//==============================================================================
void FlowFmxProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("FlowFmxHost");
    state.setProperty ("stateVersion", 1, nullptr);
    state.setProperty ("fmxPath", fmxPath, nullptr);
    state.setProperty ("fmx", fmxText, nullptr); // embedded, so projects survive a moved/deleted .fmx
    state.setProperty ("customState", juce::JSON::toString (customState, true), nullptr);
    state.setProperty ("editorWidth", editorSize.x, nullptr);
    state.setProperty ("editorHeight", editorSize.y, nullptr);

    juce::ValueTree params ("Params");
    for (auto* s : slots)
        if (s->isActive())
            params.setProperty ("p" + juce::String (s->slot), s->getValue(), nullptr);
    state.addChild (params, -1, nullptr);

    juce::MemoryOutputStream out (destData, false);
    state.writeToStream (out);
}

void FlowFmxProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto state = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! state.isValid() || ! state.hasType ("FlowFmxHost"))
        return;

    auto text = state["fmx"].toString();
    const auto path = state["fmxPath"].toString();

    if (text.isEmpty() && path.isNotEmpty() && juce::File (path).existsAsFile())
        text = juce::File (path).loadFileAsString();

    if (text.isEmpty())
        return;

    // restore parameter values and custom state first; loadFmxText() picks them up
    auto params = state.getChildWithName ("Params");
    for (auto* s : slots)
        s->setValue (static_cast<float> (params.getProperty ("p" + juce::String (s->slot), s->getValue())));

    editorSize = { (int) state.getProperty ("editorWidth", 0), (int) state.getProperty ("editorHeight", 0) };

    auto cs = juce::JSON::parse (state["customState"].toString());
    customState = cs.isObject() ? cs : juce::var (new juce::DynamicObject());

    auto doLoad = [this, text, path]
    {
        auto r = loadFmxText (text, path, false);
        if (r.failed())
        {
            statusText = r.getErrorMessage();
            sendChangeMessage();
        }
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        doLoad();
    else
        juce::MessageManager::callAsync (doLoad);
}

//==============================================================================
juce::AudioProcessorEditor* FlowFmxProcessor::createEditor()
{
    return new FlowFmxEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FlowFmxProcessor();
}
