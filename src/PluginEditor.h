#pragma once

#include "PluginProcessor.h"

#include <juce_gui_extra/juce_gui_extra.h>

/** The plugin window: a slim native top bar ("Load .fmx...") and, once an .fmx is loaded,
    its own HTML UI running in WebView2, wired to the processor through a small bridge
    that emulates the iPlug2 messages the Flow UI expects. */
class FlowFmxEditor : public juce::AudioProcessorEditor,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    explicit FlowFmxEditor (FlowFmxProcessor&);
    ~FlowFmxEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // drag & drop an .fmx onto the window (works on the native parts of the window)
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void chooseFmx();
    void loadFmx (const juce::File&);
    void rebuildWebView();
    juce::String buildBridgeScript() const;

    void handleUiMessage (const juce::var& msg);
    void chooseAudioFile (const juce::String& title, const juce::String& replyFunction);
    void handleFilesDroppedOnPage (const juce::var& payload);
    void answerDropRequest (int attempt);
    void callJs (const juce::String& function, const juce::var& argument);
    void sendAllParams();

    FlowFmxProcessor& processor;

    juce::TextButton loadButton { "Load Your Plugin" };
    juce::TextButton closeButton { "Close plugin" };
    juce::Label versionLabel;
    juce::Label statusLabel;
    std::unique_ptr<juce::WebBrowserComponent> web;
    std::unique_ptr<juce::FileChooser> chooser;

    std::array<float, FlowFmxProcessor::numSlots> lastSent {};
    uint32_t lastTelemetryCounter = 0;
    juce::String lastTransportJson;
    int tick = 0;
    bool pageReady = false;
    bool sizedForCurrentFmx = false;

    // file drops onto the web UI (paths delivered by the patched WebView2 backend)
    juce::String droppedPath;
    double droppedPathTime = 0.0, fmxDropTime = 0.0;

    static constexpr int topBarHeight = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlowFmxEditor)
};
