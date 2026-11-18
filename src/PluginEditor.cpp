#include "PluginEditor.h"

#ifndef FMX_BUILD_ID
 #define FMX_BUILD_ID "dev"
#endif

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

    closeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff26262c));
    closeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.8f));
    closeButton.setTooltip ("Unload the current plugin and go back to the start screen");
    closeButton.onClick = [this] { processor.unloadFmx(); };
    addChildComponent (closeButton);

    versionLabel.setText (juce::String::fromUTF8 (JucePlugin_Manufacturer "  \xc2\xb7  Flow FMX Host v" JucePlugin_VersionString " \xc2\xb7 " FMX_BUILD_ID), juce::dontSendNotification);
    versionLabel.setFont (juce::FontOptions (10.5f));
    versionLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.3f));
    versionLabel.setJustificationType (juce::Justification::centredRight);
    versionLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (versionLabel);

    statusLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.75f));
    statusLabel.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (statusLabel);

    processor.addChangeListener (this);

    setResizable (true, true);
    setResizeLimits (480, 320, 2400, 1800);
    if (processor.editorSize.x > 0 && processor.editorSize.y > 0)
    {
        sizedForCurrentFmx = true;
        setSize (processor.editorSize.x, processor.editorSize.y);
    }
    else
    {
        setSize (800, 600 + topBarHeight);
    }

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

        auto centre = area.withSizeKeepingCentre (area.getWidth(), 150);

        g.setColour (accent);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawFittedText (juce::String (JucePlugin_Name).toUpperCase(), centre.removeFromTop (24), juce::Justification::centred, 1);

        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        g.drawFittedText ("Drop a Google Flow .fmx file here", centre.removeFromTop (44), juce::Justification::centred, 1);

        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("or click \"Load Your Plugin\" above", centre.removeFromTop (26), juce::Justification::centred, 1);

        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.setFont (juce::FontOptions (12.0f));
        g.drawFittedText (FMX_IS_SYNTH ? "For Flow instruments. Effects go in \"Flow FMX Effect\"."
                                       : "For Flow effects. Instruments go in \"Flow FMX Instrument\".",
                          centre.removeFromTop (40), juce::Justification::centredBottom, 1);
    }
}

void FlowFmxEditor::resized()
{
    auto bar = getLocalBounds().removeFromTop (topBarHeight).reduced (4, 3);
    loadButton.setBounds (bar.removeFromLeft (124));

    if (closeButton.isVisible())
    {
        bar.removeFromLeft (6);
        closeButton.setBounds (bar.removeFromLeft (100));
    }

    versionLabel.setBounds (bar.removeFromRight (juce::jmin (300, bar.getWidth() / 2 - 40)));
    bar.removeFromLeft (10);
    statusLabel.setBounds (bar);

    if (web != nullptr)
        web->setBounds (getLocalBounds().withTrimmedTop (topBarHeight));

    if (sizedForCurrentFmx)
        processor.editorSize = { getWidth(), getHeight() };
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
  // No browser right-click menu (Back / Reload / Inspect ...) inside the plugin. Only the
  // default action is blocked, so a Flow UI's own right-click handling keeps working.
  window.addEventListener("contextmenu", function (e) { e.preventDefault(); }, true);
  // Stop WebView2 from navigating away when a file is dropped, and hand the dropped files'
  // real paths to the host. A page can't read file paths itself, but WebView2 passes them to
  // the host for File objects sent with postMessageWithAdditionalObjects(). This runs in the
  // capture phase, i.e. before the Flow UI's own drop handler asks for the path.
  window.addEventListener("dragover", function (e) { e.preventDefault(); });
  window.addEventListener("drop", function (e) {
    e.preventDefault();
    var files = e.dataTransfer && e.dataTransfer.files;
    var wv = window.chrome && window.chrome.webview;
    if (files && files.length > 0 && wv && typeof wv.postMessageWithAdditionalObjects === "function") {
      try {
        wv.postMessageWithAdditionalObjects(
          JSON.stringify({ eventId: "fmxDrop", payload: { count: files.length } }), files);
      } catch (err) { console.error(err); }
    }
  }, true);
  document.addEventListener("DOMContentLoaded", function () {
    window.IPlugSendMsg({ msg: "__fmxReady" });
    startFitToWindow();
  });

  // Flow UIs are designed at a fixed size (e.g. 800x600). Scale the content to fit the
  // plugin window, centred, and tell the host the natural size once it's known.
  var lastZoom = 0, lastW = 0, lastH = 0;
  function measure() {
    var root = document.getElementById("root");
    if (!root) return null;
    // the content's real edges relative to its container, in unzoomed CSS pixels, so neither
    // the zoom nor the centring padding below feed back into the result
    var host = root, hr = host.getBoundingClientRect(), z = lastZoom || 1;
    var l = Infinity, t = Infinity, r = -Infinity, b = -Infinity;
    for (var i = 0; i < host.children.length; i++) {
      var c = host.children[i].getBoundingClientRect();
      if (c.width <= 0 || c.height <= 0) continue;
      l = Math.min(l, c.left); t = Math.min(t, c.top); r = Math.max(r, c.right); b = Math.max(b, c.bottom);
    }
    if (!isFinite(l)) return null;
    return { w: Math.round((r - l) / z), h: Math.round((b - t) / z),
             x: (l - hr.left) / z, y: (t - hr.top) / z };
  }
  function fit() {
    var m = measure();
    if (!m || m.w <= 0 || m.h <= 0) return;
    if (Math.abs(m.w - lastW) > 2 || Math.abs(m.h - lastH) > 2) {
      lastW = m.w; lastH = m.h;
      window.IPlugSendMsg({ msg: "__fmxSize", width: m.w, height: m.h });
    }
    var z = Math.min(window.innerWidth / lastW, window.innerHeight / lastH);
    if (!isFinite(z) || z <= 0) return;
    if (Math.abs(z - lastZoom) > 0.0005) { document.documentElement.style.zoom = String(z); lastZoom = z; }
    var px = (window.innerWidth  - lastW * z) / 2 / z - m.x;
    var py = (window.innerHeight - lastH * z) / 2 / z - m.y;
    var bs = document.body.style, nl = px.toFixed(1) + "px", nt = py.toFixed(1) + "px";
    if (bs.marginLeft !== nl) bs.marginLeft = nl;
    if (bs.marginTop !== nt) bs.marginTop = nt;
  }
  function startFitToWindow() {
    var css = document.createElement("style");
    css.textContent = "html{overflow:hidden;background:#0b0b0e}body{margin:0;overflow:hidden}";
    document.head.appendChild(css);
    window.addEventListener("resize", function () { requestAnimationFrame(fit); });
    // no ResizeObserver: some Flow UIs report its harmless loop warning as a crash
    var n = 0;
    (function poll() { fit(); setTimeout(poll, ++n < 20 ? 100 : 500); })();
  }
})();
)JS")
        .replace ("%META%", toJs (juce::var (meta)))
        .replace ("%CUSTOM%", hasCustom ? toJs (custom) : juce::String ("undefined"));
}

void FlowFmxEditor::rebuildWebView()
{
    statusLabel.setText (processor.getStatusText(), juce::dontSendNotification);
    closeButton.setVisible (processor.hasFmx());
    droppedPath.clear();
    resized();

    if (web != nullptr)
    {
        removeChildComponent (web.get());
        web.reset();
    }

    pageReady = false;
    sizedForCurrentFmx = processor.editorSize.x > 0;
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
        .withEventListener ("fmxDrop", [safe = juce::Component::SafePointer<FlowFmxEditor> (this)] (juce::var payload)
        {
            if (safe != nullptr)
                safe->handleFilesDroppedOnPage (payload);
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
        // The UI's drop zone wants the dropped file's path. Our capture-phase drop handler has
        // normally delivered it already (handleFilesDroppedOnPage); give it a moment otherwise.
        answerDropRequest (0);
    }
    else if (type == "vstInspectAudioFile")
    {
        const int bins = m.hasProperty ("peakBins") ? (int) m["peakBins"] : 256;
        callJs ("__vstOnAudioFileInspected", processor.inspectAudioFile (m["path"].toString(), bins));
    }
    else if (type == "__fmxSize")
    {
        // first size report after loading an .fmx: open the window at the UI's natural size
        const int w = (int) m["width"], h = (int) m["height"];
        if (! sizedForCurrentFmx && w >= 200 && h >= 150 && w <= 2400 && h <= 1800)
        {
            sizedForCurrentFmx = true;
            setSize (w, h + topBarHeight);
        }
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

void FlowFmxEditor::handleFilesDroppedOnPage (const juce::var& payload)
{
    auto* paths = payload["paths"].getArray();
    if (paths == nullptr || paths->isEmpty())
        return; // old WebView2 runtime: answerDropRequest() falls back to a file chooser

    const auto now = juce::Time::getMillisecondCounterHiRes();

    // an .fmx dropped anywhere on the UI loads it
    for (auto& p : *paths)
    {
        if (p.toString().endsWithIgnoreCase (".fmx"))
        {
            fmxDropTime = now;
            const juce::File file (p.toString());
            // deferred: we're inside a WebView callback and loading replaces the WebView
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<FlowFmxEditor> (this), file]
            {
                if (safe != nullptr)
                    safe->loadFmx (file);
            });
            return;
        }
    }

    droppedPath = (*paths)[0].toString();
    droppedPathTime = now;
}

void FlowFmxEditor::answerDropRequest (int attempt)
{
    const auto now = juce::Time::getMillisecondCounterHiRes();

    if (droppedPath.isNotEmpty() && now - droppedPathTime < 3000.0)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("path", droppedPath);
        droppedPath.clear();
        callJs ("__vstOnAudioFileDropped", juce::var (o));
        return;
    }

    if (now - fmxDropTime < 3000.0)
        return; // an .fmx was dropped onto the sample area: it's being loaded instead

    if (attempt < 5)
    {
        juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<FlowFmxEditor> (this), attempt]
        {
            if (safe != nullptr)
                safe->answerDropRequest (attempt + 1);
        });
        return;
    }

    // the path never arrived (very old WebView2 runtime): ask with a file chooser instead
    chooseAudioFile ("Select audio sample", "__vstOnAudioFileDropped");
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
