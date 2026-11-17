cmake_minimum_required(VERSION 3.22)

project(FlowFmxHost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the C/C++ runtime statically on Windows, so the plugin needs no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(FMX_BUILD_PLUGINS "Build the VST3 plugins (needs a desktop platform)" ON)
option(FMX_BUILD_TOOLS   "Build the headless fmx_render test tool" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- JUCE --------------------------------------------------------------------
# (set FETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to use a local checkout)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.17-security
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# lets dropped files reach the plugin with their real path (WebView2 only)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuce.cmake)

# --- wasm3 (WebAssembly interpreter that runs the .fmx audio engine) ----------
FetchContent_Declare(wasm3
    GIT_REPOSITORY https://github.com/wasm3/wasm3.git
    GIT_TAG        28ecb9af6d2040e474a70f7cb7f43666740141fb
    SOURCE_SUBDIR  source)
FetchContent_MakeAvailable(wasm3)

set_target_properties(m3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(m3 PRIVATE d_m3HasTracer=0)
if(MSVC)
    target_compile_definitions(m3 PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "MSVC")
        target_compile_options(m3 PRIVATE /Zc:preprocessor)
    endif()
    target_compile_options(m3 PRIVATE $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(m3 PRIVATE $<$<CONFIG:Release>:-O3> -Wno-unused-function -Wno-unused-parameter)
endif()

# --- shared engine code -------------------------------------------------------
# Compiled into each target (JUCE modules must not be linked through a static library).
add_library(fmx_engine INTERFACE)
target_sources(fmx_engine INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/FmxFile.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AudioFileCache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/WasmDsp.cpp)
target_include_directories(fmx_engine INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fmx_engine INTERFACE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_audio_formats
    m3)
target_compile_definitions(fmx_engine INTERFACE
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# --- plugins ------------------------------------------------------------------
function(fmx_add_plugin target product code is_synth categories)
    juce_add_plugin(${target}
        COMPANY_NAME            "FlowFmxHost"
        BUNDLE_ID               "com.flowfmxhost.${target}"
        PLUGIN_MANUFACTURER_CODE Ffmx
        PLUGIN_CODE             ${code}
        PRODUCT_NAME            "${product}"
        FORMATS                 VST3
        IS_SYNTH                ${is_synth}
        NEEDS_MIDI_INPUT        ${is_synth}
        NEEDS_MIDI_OUTPUT       FALSE
        IS_MIDI_EFFECT          FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        NEEDS_WEBVIEW2          TRUE
        NEEDS_WEB_BROWSER       TRUE
        NEEDS_CURL              FALSE
        VST3_CATEGORIES         ${categories}
        COPY_PLUGIN_AFTER_BUILD FALSE)

    target_sources(${target} PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp)

    target_compile_definitions(${target} PUBLIC
        FMX_IS_SYNTH=$<BOOL:${is_synth}>
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)

    target_link_libraries(${target}
        PRIVATE
            fmx_engine
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endfunction()

if(FMX_BUILD_PLUGINS)
    fmx_add_plugin(FlowFmxInstrument "Flow FMX Instrument" Fmxi TRUE  "Instrument;Sampler")
    fmx_add_plugin(FlowFmxEffect     "Flow FMX Effect"     Fmxe FALSE "Fx")
endif()

# --- headless test tool -------------------------------------------------------
if(FMX_BUILD_TOOLS)
    juce_add_console_app(fmx_render PRODUCT_NAME "fmx_render")
    target_sources(fmx_render PRIVATE tools/fmx_render.cpp)
    target_link_libraries(fmx_render PRIVATE fmx_engine juce::juce_recommended_config_flags)
    target_compile_definitions(fmx_render PRIVATE JUCE_WEB_BROWSER=0)
endif()

# --- processor end-to-end test (links the instrument's shared code) -------------
if(FMX_BUILD_PLUGINS AND FMX_BUILD_TOOLS)
    add_executable(fmx_plugin_test tests/plugin_test.cpp)
    target_include_directories(fmx_plugin_test PRIVATE
        src $<TARGET_PROPERTY:FlowFmxInstrument,INCLUDE_DIRECTORIES>)
    target_compile_definitions(fmx_plugin_test PRIVATE
        $<TARGET_PROPERTY:FlowFmxInstrument,COMPILE_DEFINITIONS>)
    target_link_libraries(fmx_plugin_test PRIVATE FlowFmxInstrument)
endif()
