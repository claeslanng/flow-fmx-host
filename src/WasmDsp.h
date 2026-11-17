#pragma once

#include "AudioFileCache.h"
#include "FmxFile.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// wasm3 forward declarations (keeps wasm3.h out of other translation units)
struct M3Environment;
struct M3Runtime;
struct M3Module;
struct M3Function;

namespace fmx
{

/** Runs the WebAssembly DSP contained in an .fmx file (Flow plugin ABI v1).

    ABI v1, as reverse engineered from Flow-generated modules:
      exports  init(f32 sampleRate), onStateChanged(), getNumParams() -> i32,
               noteOn(i32 note, i32 velocity), noteOff(i32 note),
               process(i32 inL, i32 inR, i32 outL, i32 outR, i32 numFrames),
               getAbiVersion() -> i32, memory
      imports  env.abort, env.hostExposeBuffer, env.getStateString,
               env.hostOpenAudioFile + hostGetAudioFile{SampleRate,Channels,Frames},
               env.hostReadAudioFileChannel
      memory   plain (denormalised) parameter values live as f32 at 65536 + 4 * id.
               The first 64 KiB are free for the host's audio I/O buffers (4 x 4096 floats).

    Threading: create/init/stateChanged run on the message thread, process/noteOn/noteOff on
    the audio thread. The owner must make sure they never run at the same time (see the
    processor's dspLock).
*/
class WasmDsp
{
public:
    static constexpr int maxFramesPerCall = 4096;
    static constexpr int maxParams        = 64;
    static constexpr uint32_t inLOffset   = 0;
    static constexpr uint32_t inROffset   = 16384;
    static constexpr uint32_t outLOffset  = 32768;
    static constexpr uint32_t outROffset  = 49152;
    static constexpr uint32_t paramOffset = 65536;

    using AudioProvider = std::function<std::shared_ptr<const DecodedAudio> (const juce::String& path)>;

    ~WasmDsp();

    /** Parses/links the module. Returns nullptr and sets error on failure. */
    static std::unique_ptr<WasmDsp> create (const juce::MemoryBlock& wasm, juce::String& error);

    void setAudioProvider (AudioProvider p) { audioProvider = std::move (p); }

    /** Key/value strings handed to the DSP through getStateString(). */
    void setStateStrings (std::map<std::string, std::string> s) { stateStrings = std::move (s); }

    bool init (double sampleRate);
    bool stateChanged();
    int  getNumParams();
    int  getAbiVersion() const { return abiVersion; }

    // audio thread
    void setParam (int id, float plainValue);
    void noteOn (int note, int velocity);
    void noteOff (int note);
    /** Runs the DSP. Any numFrames is fine, it gets split internally. in* may be null. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numFrames);

    bool hasFailed() const          { return failed.load(); }
    juce::String getError() const   { return errorText; }

    /** Buffers the DSP registered with hostExposeBuffer() (e.g. "telemetry"). */
    struct ExposedBuffer
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    const std::vector<ExposedBuffer>& getExposedBuffers() const { return exposed; }

    /** Copies an exposed buffer's current contents (call from the audio thread right after process). */
    int readExposed (const ExposedBuffer& b, float* dest, int maxCount);

private:
    WasmDsp() = default;
    bool check (const char* result, const char* what);
    uint8_t* memory (size_t& size);

    // host import implementations (static trampolines live in the .cpp)
    friend struct HostImports;

    std::vector<uint8_t> wasmBytes; // must outlive the module
    M3Environment* env = nullptr;
    M3Runtime* runtime = nullptr;
    M3Module* module = nullptr;

    M3Function* fnInit = nullptr;
    M3Function* fnStateChanged = nullptr;
    M3Function* fnGetNumParams = nullptr;
    M3Function* fnNoteOn = nullptr;
    M3Function* fnNoteOff = nullptr;
    M3Function* fnProcess = nullptr;
    M3Function* fnAbi = nullptr;

    int abiVersion = 0;
    std::atomic<bool> failed { false };
    juce::String errorText;

    AudioProvider audioProvider;
    std::shared_ptr<const DecodedAudio> openedFile;
    std::map<std::string, std::string> stateStrings;
    std::vector<ExposedBuffer> exposed;
};

} // namespace fmx
