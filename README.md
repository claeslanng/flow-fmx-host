# Flow FMX Host

A Windows **VST3 plugin that loads Google Flow Music `.fmx` plugins**, so they can be used in
FL Studio (or any VST3 host) on Windows.

- **Flow FMX Instrument**: hosts Flow instruments (e.g. *Studio Sampler*)
- **Flow FMX Effect**: hosts Flow effects (experimental, untested with real effect files)

**Download:** see the [latest build](../../releases/tag/latest) and `docs/INSTALL.txt`.

## How it works

An `.fmx` file is JSON with three important parts:

| field      | contents                                                         |
|------------|------------------------------------------------------------------|
| `wasm`     | gzip+base64 WebAssembly module: the audio engine (DSP)          |
| `html`     | gzip+base64 single-page UI (React), written against iPlug2's WebView bridge |
| `metadata` | JSON parameter list (name, range, curve, unit, choices)          |

This host:

1. runs the `wasm` module with the [wasm3](https://github.com/wasm3/wasm3) interpreter
   (`src/WasmDsp.*`, ABI v1 documented in `WasmDsp.h`),
2. shows the `html` UI in WebView2 via JUCE's `WebBrowserComponent`, with a small script that
   emulates the iPlug2 messages the UI expects (`src/PluginEditor.cpp`),
3. exposes the parameters to the DAW for automation (64 fixed slots), handles MIDI, and stores
   the whole `.fmx` plus parameter values and the UI's custom state in the DAW project.

## Building

GitHub Actions builds the Windows plugins on every push to `main` (`.github/workflows/build.yml`),
runs `fmx_plugin_test` against `tests/fixtures/Studio_Sampler_v8.fmx`, and publishes the zip as
the `latest` release.

Local build (Windows, Visual Studio 2022 with the "C++ Clang tools" component):

```bat
nuget install Microsoft.Web.WebView2 -OutputDirectory C:\nuget
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -T ClangCL -DJUCE_WEBVIEW2_PACKAGE_LOCATION=C:/nuget
cmake --build build --config Release
```

On Linux, the engine test tools build too:
`fmx_render <file.fmx> <out.wav>` renders a chord to a WAV file.

## Licensing note

Built with JUCE 8, which is licensed under AGPLv3 or a commercial JUCE licence. Fine for personal
use; if you distribute the plugin, the AGPL (or a JUCE licence) applies.
