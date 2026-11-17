#include "WasmDsp.h"

#include <wasm3.h>

#include <cstring>

namespace fmx
{

//==============================================================================
// Host functions imported by the module ("env" namespace)
//==============================================================================
struct HostImports
{
    static WasmDsp& self (IM3ImportContext ctx) { return *static_cast<WasmDsp*> (ctx->userdata); }

    static bool inBounds (void* mem, uint32_t offset, uint64_t len)
    {
        const auto size = m3_GetMemorySizeAt (mem);
        return (uint64_t) offset <= size && len <= (uint64_t) size - offset;
    }

    static std::string readString (void* mem, uint32_t ptr, uint32_t len)
    {
        if (! inBounds (mem, ptr, len))
            return {};
        return std::string ((const char*) mem + ptr, len);
    }

    // abort(msgPtr, filePtr, line, col)  -- AssemblyScript runtime abort
    static m3ApiRawFunction (abort_)
    {
        m3ApiGetArg (int32_t, msg)
        m3ApiGetArg (int32_t, file)
        m3ApiGetArg (int32_t, line)
        m3ApiGetArg (int32_t, col)
        DBG ("fmx dsp abort at line " << line << ":" << col);
        juce::ignoreUnused (msg, file, line, col);
        m3ApiTrap (m3Err_trapAbort)
    }

    // hostExposeBuffer(namePtr, nameLen, dataPtr, count) -> i32
    static m3ApiRawFunction (hostExposeBuffer)
    {
        m3ApiReturnType (int32_t)
        m3ApiGetArg (uint32_t, namePtr)
        m3ApiGetArg (uint32_t, nameLen)
        m3ApiGetArg (uint32_t, dataPtr)
        m3ApiGetArg (uint32_t, count)

        auto& dsp = self (_ctx);
        const auto name = readString (_mem, namePtr, nameLen);

        if (name.empty() || ! inBounds (_mem, dataPtr, (uint64_t) count * 4))
            m3ApiReturn (0)

        for (auto& b : dsp.exposed)
        {
            if (b.name == name)
            {
                b.offset = dataPtr;
                b.count = count;
                m3ApiReturn (1)
            }
        }

        if (dsp.exposed.size() < 16)
            dsp.exposed.push_back ({ name, dataPtr, count });

        m3ApiReturn (1)
    }

    // getStateString(keyPtr, keyLen, bufPtr, bufLen) -> bytes written (0 if missing)
    static m3ApiRawFunction (getStateString)
    {
        m3ApiReturnType (int32_t)
        m3ApiGetArg (uint32_t, keyPtr)
        m3ApiGetArg (uint32_t, keyLen)
        m3ApiGetArg (uint32_t, bufPtr)
        m3ApiGetArg (uint32_t, bufLen)

        auto& dsp = self (_ctx);
        const auto key = readString (_mem, keyPtr, keyLen);
        auto it = dsp.stateStrings.find (key);

        if (it == dsp.stateStrings.end() || ! inBounds (_mem, bufPtr, bufLen))
            m3ApiReturn (0)

        const auto n = (uint32_t) juce::jmin ((size_t) bufLen, it->second.size());
        std::memcpy ((uint8_t*) _mem + bufPtr, it->second.data(), n);
        m3ApiReturn ((int32_t) n)
    }

    // hostOpenAudioFile(pathPtr, pathLen) -> 1 on success
    static m3ApiRawFunction (hostOpenAudioFile)
    {
        m3ApiReturnType (int32_t)
        m3ApiGetArg (uint32_t, pathPtr)
        m3ApiGetArg (uint32_t, pathLen)

        auto& dsp = self (_ctx);
        const auto path = readString (_mem, pathPtr, pathLen);
        dsp.openedFile = nullptr;

        if (! path.empty() && dsp.audioProvider)
            dsp.openedFile = dsp.audioProvider (juce::String::fromUTF8 (path.data(), (int) path.size()));

        m3ApiReturn (dsp.openedFile != nullptr ? 1 : 0)
    }

    static m3ApiRawFunction (hostGetAudioFileSampleRate)
    {
        m3ApiReturnType (float)
        auto& dsp = self (_ctx);
        m3ApiReturn (dsp.openedFile ? (float) dsp.openedFile->sampleRate : 44100.0f)
    }

    static m3ApiRawFunction (hostGetAudioFileChannels)
    {
        m3ApiReturnType (int32_t)
        auto& dsp = self (_ctx);
        m3ApiReturn (dsp.openedFile ? dsp.openedFile->buffer.getNumChannels() : 0)
    }

    static m3ApiRawFunction (hostGetAudioFileFrames)
    {
        m3ApiReturnType (int32_t)
        auto& dsp = self (_ctx);
        m3ApiReturn (dsp.openedFile ? dsp.openedFile->buffer.getNumSamples() : 0)
    }

    // hostReadAudioFileChannel(channel, destPtr, numFrames) -> frames copied
    static m3ApiRawFunction (hostReadAudioFileChannel)
    {
        m3ApiReturnType (int32_t)
        m3ApiGetArg (int32_t, channel)
        m3ApiGetArg (uint32_t, destPtr)
        m3ApiGetArg (int32_t, numFrames)

        auto& dsp = self (_ctx);
        if (dsp.openedFile == nullptr || numFrames <= 0)
            m3ApiReturn (0)

        auto& buf = dsp.openedFile->buffer;
        const int ch = juce::jlimit (0, buf.getNumChannels() - 1, (int) channel);
        const int n = juce::jmin ((int) numFrames, buf.getNumSamples());

        if (! inBounds (_mem, destPtr, (uint64_t) n * 4))
            m3ApiTrap (m3Err_trapOutOfBoundsMemoryAccess)

        std::memcpy ((uint8_t*) _mem + destPtr, buf.getReadPointer (ch), (size_t) n * sizeof (float));
        m3ApiReturn (n)
    }
};

//==============================================================================
WasmDsp::~WasmDsp()
{
    if (runtime != nullptr) m3_FreeRuntime (runtime); // also frees loaded modules
    else if (module != nullptr) m3_FreeModule (module);
    if (env != nullptr) m3_FreeEnvironment (env);
}

bool WasmDsp::check (const char* result, const char* what)
{
    if (result == m3Err_none)
        return true;

    errorText = juce::String (what) + ": " + result;

    if (runtime != nullptr)
    {
        M3ErrorInfo info {};
        m3_GetErrorInfo (runtime, &info);
        if (info.message != nullptr && *info.message != 0)
            errorText << " (" << info.message << ")";
    }

    DBG ("fmx: " << errorText);
    failed = true;
    return false;
}

std::unique_ptr<WasmDsp> WasmDsp::create (const juce::MemoryBlock& wasm, juce::String& error)
{
    std::unique_ptr<WasmDsp> d (new WasmDsp());
    d->wasmBytes.assign ((const uint8_t*) wasm.getData(), (const uint8_t*) wasm.getData() + wasm.getSize());

    d->env = m3_NewEnvironment();
    d->runtime = d->env != nullptr ? m3_NewRuntime (d->env, 256 * 1024, nullptr) : nullptr;

    if (d->runtime == nullptr)
    {
        error = "Could not create the WebAssembly runtime";
        return nullptr;
    }

    IM3Module mod = nullptr;
    if (! d->check (m3_ParseModule (d->env, &mod, d->wasmBytes.data(), (uint32_t) d->wasmBytes.size()), "parse"))
    {
        error = d->errorText;
        return nullptr;
    }

    if (! d->check (m3_LoadModule (d->runtime, mod), "load"))
    {
        m3_FreeModule (mod);
        error = d->errorText;
        return nullptr;
    }

    d->module = mod;

    struct Link { const char* name; const char* sig; M3RawCall fn; };
    const Link links[] = {
        { "abort",                      "v(iiii)", &HostImports::abort_ },
        { "hostExposeBuffer",           "i(iiii)", &HostImports::hostExposeBuffer },
        { "getStateString",             "i(iiii)", &HostImports::getStateString },
        { "hostOpenAudioFile",          "i(ii)",   &HostImports::hostOpenAudioFile },
        { "hostGetAudioFileSampleRate", "f()",     &HostImports::hostGetAudioFileSampleRate },
        { "hostGetAudioFileChannels",   "i()",     &HostImports::hostGetAudioFileChannels },
        { "hostGetAudioFileFrames",     "i()",     &HostImports::hostGetAudioFileFrames },
        { "hostReadAudioFileChannel",   "i(iii)",  &HostImports::hostReadAudioFileChannel },
    };

    for (auto& l : links)
    {
        auto r = m3_LinkRawFunctionEx (mod, "env", l.name, l.sig, l.fn, d.get());
        // modules that don't import a given function report "function lookup failed"; that's fine
        if (r != m3Err_none && r != m3Err_functionLookupFailed)
        {
            d->check (r, l.name);
            error = d->errorText;
            return nullptr;
        }
    }

    auto find = [&d] (IM3Function& f, const char* name, bool required)
    {
        auto r = m3_FindFunction (&f, d->runtime, name);
        if (r == m3Err_none) return true;
        f = nullptr;
        if (required) d->check (r, name);
        return ! required;
    };

    if (! find (d->fnInit, "init", true)
        || ! find (d->fnProcess, "process", true)
        || ! find (d->fnStateChanged, "onStateChanged", false)
        || ! find (d->fnGetNumParams, "getNumParams", false)
        || ! find (d->fnNoteOn, "noteOn", false)
        || ! find (d->fnNoteOff, "noteOff", false)
        || ! find (d->fnAbi, "getAbiVersion", false))
    {
        error = d->errorText;
        return nullptr;
    }

    if (d->fnAbi != nullptr && m3_CallV (d->fnAbi) == m3Err_none)
    {
        int32_t v = 0;
        m3_GetResultsV (d->fnAbi, &v);
        d->abiVersion = v;
    }

    size_t memSize = 0;
    if (d->memory (memSize) == nullptr || memSize < paramOffset + maxParams * 4)
    {
        error = "The audio engine has no (or too little) memory";
        return nullptr;
    }

    return d;
}

uint8_t* WasmDsp::memory (size_t& size)
{
    return m3_GetMemory (module, &size, 0);
}

bool WasmDsp::init (double sampleRate)
{
    if (failed) return false;
    exposed.clear();
    return check (m3_CallV (fnInit, (double) (float) sampleRate), "init");
}

bool WasmDsp::stateChanged()
{
    if (failed || fnStateChanged == nullptr) return ! failed;
    return check (m3_CallV (fnStateChanged), "onStateChanged");
}

int WasmDsp::getNumParams()
{
    if (failed || fnGetNumParams == nullptr) return -1;
    if (! check (m3_CallV (fnGetNumParams), "getNumParams")) return -1;
    int32_t n = 0;
    m3_GetResultsV (fnGetNumParams, &n);
    return n;
}

void WasmDsp::setParam (int id, float plainValue)
{
    if (! juce::isPositiveAndBelow (id, maxParams)) return;
    size_t size = 0;
    if (auto* mem = memory (size))
        std::memcpy (mem + paramOffset + (uint32_t) id * 4, &plainValue, 4);
}

void WasmDsp::noteOn (int note, int velocity)
{
    if (failed || fnNoteOn == nullptr) return;
    check (m3_CallV (fnNoteOn, (int32_t) note, (int32_t) velocity), "noteOn");
}

void WasmDsp::noteOff (int note)
{
    if (failed || fnNoteOff == nullptr) return;
    check (m3_CallV (fnNoteOff, (int32_t) note), "noteOff");
}

void WasmDsp::process (const float* inL, const float* inR, float* outL, float* outR, int numFrames)
{
    int done = 0;

    while (done < numFrames && ! failed)
    {
        const int n = juce::jmin (maxFramesPerCall, numFrames - done);
        size_t size = 0;
        auto* mem = memory (size);

        if (inL != nullptr) std::memcpy (mem + inLOffset, inL + done, (size_t) n * 4);
        else                std::memset (mem + inLOffset, 0, (size_t) n * 4);
        if (inR != nullptr) std::memcpy (mem + inROffset, inR + done, (size_t) n * 4);
        else if (inL != nullptr) std::memcpy (mem + inROffset, inL + done, (size_t) n * 4);
        else                std::memset (mem + inROffset, 0, (size_t) n * 4);

        if (! check (m3_CallV (fnProcess, (int32_t) inLOffset, (int32_t) inROffset,
                               (int32_t) outLOffset, (int32_t) outROffset, (int32_t) n), "process"))
            break;

        mem = memory (size); // memory may have grown during the call
        std::memcpy (outL + done, mem + outLOffset, (size_t) n * 4);
        std::memcpy (outR + done, mem + outROffset, (size_t) n * 4);
        done += n;
    }

    if (done < numFrames)
    {
        std::memset (outL + done, 0, (size_t) (numFrames - done) * 4);
        std::memset (outR + done, 0, (size_t) (numFrames - done) * 4);
    }
}

int WasmDsp::readExposed (const ExposedBuffer& b, float* dest, int maxCount)
{
    size_t size = 0;
    auto* mem = memory (size);
    const int n = juce::jmin ((int) b.count, maxCount);

    if (mem == nullptr || (uint64_t) b.offset + (uint64_t) n * 4 > size)
        return 0;

    std::memcpy (dest, mem + b.offset, (size_t) n * 4);
    return n;
}

} // namespace fmx
