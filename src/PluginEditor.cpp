#include "PluginEditor.h"

namespace
{
    const juce::Colour background (0xff0b0b0e);
    const juce::Colour barColour  (0xff141418);
    const juce::Colour accent     (0xff10b981);

    juce::String toJs (const juce::var& v)
    {
        return juce::JSON::toString (v, true);
    }

    juce::File webViewDataFolder()
    {
       #if JUCE_WINDOWS
        auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);
       #else
        auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
       #endif
        return base.getChildFile ("FlowFmxHost").getChildFile ("WebView2");
    }
}

//==============================================================================
FlowFmxEditor::FlowFmxEditor (FlowFmxProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    lastSent.fill (-1.0f);

    loadButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff1c2a24));
    loadButton.setColour (juce::TextButton::textColourOffId, accent.brighter (0.4f));
    loadButton.onClick = [this] { chooseFmx(); };
    addAndMakeVisible (loadButton);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);

    processor.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (480, 320, 2400, 1800);
    setSize (820, 600 + topBarHeight);

    rebuildWebView();
    startTimerHz (30);
}

FlowFmxEditor::~FlowFmxEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
}

//==============================================================================
void FlowFmxEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);
    g.setColour (barColour);
    g.fillRect (getLocalBounds().removeFromTop (topBarHeight));

    if (web == nullptr)
    {
        auto area = getLocalBounds().withTrimmedTop (topBarHeight).reduced (40);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (area.toFloat(), 12.0f, 2.0f);

        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        g.drawFittedText ("Drop a Google Flow .fmx file here", area.withTrimmedBottom (40), juce::Justification::centred, 1);

        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("or click \"Load .fmx...\" above", area.withTrimmedTop (40), juce::Justification::centred, 1);
    }
}

void FlowFmxEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (topBarHeight).reduced (4, 3);
    loadButton.setBounds (bar.removeFromLeft (110));
    bar.removeFromLeft (10);
    statusLabel.setBounds (bar);

    if (web != nullptr)
        web->setBounds (getLocalBounds().withTrimmedTop (topBarHeight));
}

//==============================================================================
bool FlowFmxEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (f.endsWithIgnoreCase (".fmx"))
            return true;
    return false;
}

void FlowFmxEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
    {
        if (f.endsWithIgnoreCase (".fmx"))
        {
            loadFmx (juce::File (f));
            return;
        }
    }
}

void FlowFmxEditor::chooseFmx()
{
    chooser = std::make_unique<juce::FileChooser> ("Open a Google Flow plugin (.fmx)",
                                                   juce::File (processor.getFmxPath()).getParentDirectory(),
                                                   "*.fmx");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<FlowFmxEditor> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                                  safe->loadFmx (fc.getResult());
                          });
}

void FlowFmxEditor::loadFmx (const juce::File& file)
{
    auto r = processor.loadFmxFile (file);
    if (r.failed())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Could not load .fmx", r.getErrorMessage());
    }
}

void FlowFmxEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildWebView();
}

//==============================================================================
juce::String FlowFmxEditor::buildBridgeScript() const
{
    // Initial values baked into the page, so the UI shows the right state on first paint.
    auto* initialParams = new juce::DynamicObject();
    for (int i = 0; i < FlowFmxProcessor::numSlots; ++i)
        if (auto* s = const_cast<FlowFmxProcessor&> (processor).getSlot (i); s != nullptr && s->isActive())
            initialParams->setProperty (juce::String (i), s->getValue());

    auto* meta = new juce::DynamicObject();
    meta->setProperty ("initialParams", juce::var (initialParams));
    const auto custom = processor.getCustomState();
    const bool hasCustom = custom.getDynamicObject() != nullptr && ! custom.getDynamicObject()->getProperties().isEmpty();
    if (hasCustom)
        meta->setProperty ("customState", custom);

    // iPlug2-style bridge: the Flow UI calls window.IPlugSendMsg({...}); the host calls
    // window.SPVFD(), window.__vstOn*() etc. back (see FlowFmxEditor::callJs()).
    return juce::String (R"JS(
(function () {
  window.IPlugSendMsg = function (m) {
    try { window.__JUCE__.backend.emitEvent("iplug", m); } catch (e) { console.error(e); }
  };
  window.__spaceMeta = Object.assign(window.__spaceMeta || {}, %META%);
  window.__vstCustomState = %CUSTOM%;
  // stop WebView2 from navigating away when a file is dropped outside a drop zone
  window.addEventListener("dragover", function (e) { e.preventDefault(); });
  window.addEventListener("drop", function (e) { e.preventDefault(); });
  document.addEventListener("DOMContentLoaded", function () {
    window.IPlugSendMsg({ msg: "__fmxReady" });
  });
})();
)JS")
        .replace ("%META%", toJs (juce::var (meta)))
        .replace ("%CUSTOM%", hasCustom ? toJs (custom) : juce::String ("undefined"));
}

void FlowFmxEditor::rebuildWebView()
{
    statusLabel.setText (processor.getStatusText(), juce::dontSendNotification);

    if (web != nullptr)
    {
        removeChildComponent (web.get());
        web.reset();
    }

    pageReady = false;
    lastSent.fill (-1.0f);
    lastTransportJson = {};

    if (! processor.hasFmx() || processor.getFmx().html.isEmpty())
    {
        repaint();
        return;
    }

    auto html = std::make_shared<std::string> (processor.getFmx().html.toStdString());

    auto options = juce::WebBrowserComponent::Options()
        .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
        .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2()
                                     .withUserDataFolder (webViewDataFolder())
                                     .withBackgroundColour (background)
                                     .withStatusBarDisabled())
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withUserScript (buildBridgeScript())
        .withEventListener ("iplug", [safe = juce::Component::SafePointer<FlowFmxEditor> (this)] (juce::var msg)
        {
            if (safe != nullptr)
                safe->handleUiMessage (msg);
        })
        .withResourceProvider ([html] (const juce::String& path) -> std::optional<juce::WebBrowserComponent::Resource>
        {
            if (path == "/" || path == "/index.html" || path.isEmpty())
            {
                juce::WebBrowserComponent::Resource res;
                const auto* bytes = reinterpret_cast<const std::byte*> (html->data());
                res.data.assign (bytes, bytes + html->size());
                res.mimeType = "text/html";
                return res;
            }
            return std::nullopt;
        });

    web = std::make_unique<juce::WebBrowserComponent> (options);
    addAndMakeVisible (*web);
    resized();
    web->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
    repaint();
}

//==============================================================================
void FlowFmxEditor::callJs (const juce::String& function, const juce::var& argument)
{
    if (web == nullptr)
        return;

    web->evaluateJavascript ("if (typeof window." + function + " === 'function') window."
                             + function + "(" + toJs (argument) + ");");
}

void FlowFmxEditor::sendAllParams()
{
    lastSent.fill (-1.0f);
    timerCallback();
}

void FlowFmxEditor::handleUiMessage (const juce::var& m)
{
    const auto type = m["msg"].toString();

    if (type == "SPVFUI") // set parameter value from UI (normalised)
    {
        const int idx = (int) m["paramIdx"];
        if (auto* s = processor.getSlot (idx); s != nullptr && s->isActive())
        {
            const float v = juce::jlimit (0.0f, 1.0f, (float) (double) m["value"]);
            lastSent[(size_t) idx] = v;
            s->setValueNotifyingHost (v);
        }
    }
    else if (type == "BPCFUI")
    {
        if (auto* s = processor.getSlot ((int) m["paramIdx"]); s != nullptr && s->isActive())
            s->beginChangeGesture();
    }
    else if (type == "EPCFUI")
    {
        if (auto* s = processor.getSlot ((int) m["paramIdx"]); s != nullptr && s->isActive())
            s->endChangeGesture();
    }
    else if (type == "vstGetCustomState")
    {
        callJs ("__vstOnCustomState", processor.getCustomState());
    }
    else if (type == "vstSetCustomState")
    {
        processor.setCustomStateJson (m["state"].toString());
    }
    else if (type == "vstPickAudioFile")
    {
        chooseAudioFile (m["title"].toString(), "__vstOnAudioFilePicked");
    }
    else if (type == "vstDropAudioFile")
    {
        // WebView2 doesn't tell us the dropped file's path, so ask with a file chooser instead.
        chooseAudioFile ("Select audio sample", "__vstOnAudioFileDropped");
    }
    else if (type == "vstInspectAudioFile")
    {
        const int bins = m.hasProperty ("peakBins") ? (int) m["peakBins"] : 256;
        callJs ("__vstOnAudioFileInspected", processor.inspectAudioFile (m["path"].toString(), bins));
    }
    else if (type == "__fmxReady")
    {
        pageReady = true;
        callJs ("__vstOnState", processor.getHostInfo());
        sendAllParams();
    }
}

void FlowFmxEditor::chooseAudioFile (const juce::String& title, const juce::String& replyFunction)
{
    auto start = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> (title.isNotEmpty() ? title : juce::String ("Select audio file"),
                                                   start, processor.getAudioWildcard());

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<FlowFmxEditor> (this), replyFunction] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr)
                                  return;

                              auto* o = new juce::DynamicObject();
                              const auto f = fc.getResult();
                              o->setProperty ("path", f.existsAsFile() ? juce::var (f.getFullPathName()) : juce::var());
                              safe->callJs (replyFunction, juce::var (o));
                          });
}

//==============================================================================
void FlowFmxEditor::timerCallback()
{
    if (web == nullptr)
        return;

    // 1) parameter changes from the DAW (automation, generic UI) -> page
    juce::String script;
    for (int i = 0; i < FlowFmxProcessor::numSlots; ++i)
    {
        auto* s = processor.getSlot (i);
        if (s == nullptr || ! s->isActive())
            continue;

        const float v = s->getValue();
        if (std::abs (v - lastSent[(size_t) i]) > 1.0e-6f)
        {
            lastSent[(size_t) i] = v;
            script << "window.SPVFD(" << i << "," << juce::String (v, 6) << ");";
        }
    }

    if (script.isNotEmpty())
        web->evaluateJavascript ("if (typeof window.SPVFD === 'function') {" + script + "}");

    // 2) DSP buffers exposed with hostExposeBuffer() (e.g. playhead position)
    const auto counter = processor.telemetryCounter.load();
    if (counter != lastTelemetryCounter)
    {
        lastTelemetryCounter = counter;
        auto* payload = new juce::DynamicObject();

        for (int i = 0; i < processor.numTelemetry.load(); ++i)
        {
            auto& snap = processor.telemetry[(size_t) i];
            juce::Array<juce::var> values;
            const int n = snap.count.load (std::memory_order_acquire);
            for (int k = 0; k < n; ++k)
                values.add (snap.values[(size_t) k].load (std::memory_order_relaxed));
            payload->setProperty (juce::String (snap.name), values);
        }

        callJs ("__vstOnProcessorBuffers", juce::var (payload));
    }

    // 3) transport, ~10x per second when it changes
    if (++tick % 3 == 0)
    {
        auto t = processor.getTransportInfo();
        auto json = toJs (t);
        if (json != lastTransportJson)
        {
            lastTransportJson = json;
            callJs ("__vstOnTransport", t);
        }
    }
}
