# Small, idempotent patch to JUCE's WebView2 backend.
#
# Why: when a file is dropped onto the Flow UI, the page can only see the file's name, not
# its path. WebView2 can hand the real path to the host if the page sends the DOM File
# object with chrome.webview.postMessageWithAdditionalObjects(). JUCE ignores those
# "additional objects", so this patch reads them and adds their paths to the event payload
# as `payload.paths` (see the "fmxDrop" event in src/PluginEditor.cpp).

set(_file "${juce_SOURCE_DIR}/modules/juce_gui_extra/native/juce_WebBrowserComponent_windows.cpp")
file(READ "${_file}" _src)

# compare with normalised line endings, whatever the checkout uses
string(REPLACE "\r\n" "\n" _src "${_src}")

if(_src MATCHES "FMX-PATCH")
    return()
endif()

set(_old [==[if (const auto str = callMethodWithLpwstrResult (args, &ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString))
                                                         owner.impl->handleNativeEvent (JSON::fromString (*str));]==])

set(_new [==[if (const auto str = callMethodWithLpwstrResult (args, &ICoreWebView2WebMessageReceivedEventArgs::TryGetWebMessageAsString))
                                                     {
                                                         // FMX-PATCH: forward the paths of DOM File objects sent with
                                                         // chrome.webview.postMessageWithAdditionalObjects() as payload.paths
                                                         auto message = JSON::fromString (*str);
                                                         Array<var> paths;
                                                         ComSmartPtr<ICoreWebView2WebMessageReceivedEventArgs> argsPtr (args, IncrementRef::yes);
                                                         ComSmartPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;

                                                         if (SUCCEEDED (argsPtr.QueryInterface (args2)) && args2 != nullptr)
                                                         {
                                                             ComSmartPtr<ICoreWebView2ObjectCollectionView> objects;

                                                             if (SUCCEEDED (args2->get_AdditionalObjects (objects.resetAndGetPointerAddress())) && objects != nullptr)
                                                             {
                                                                 UINT32 count = 0;
                                                                 objects->get_Count (&count);

                                                                 for (UINT32 i = 0; i < count; ++i)
                                                                 {
                                                                     ComSmartPtr<IUnknown> object;
                                                                     ComSmartPtr<ICoreWebView2File> file;

                                                                     if (SUCCEEDED (objects->GetValueAtIndex (i, object.resetAndGetPointerAddress()))
                                                                         && object != nullptr
                                                                         && SUCCEEDED (object.QueryInterface (file)) && file != nullptr)
                                                                     {
                                                                         if (const auto path = callMethodWithLpwstrResult (file.get(), &ICoreWebView2File::get_Path))
                                                                             paths.add (*path);
                                                                     }
                                                                 }
                                                             }
                                                         }

                                                         if (! paths.isEmpty())
                                                             if (auto* payload = message["payload"].getDynamicObject())
                                                                 payload->setProperty ("paths", paths);

                                                         owner.impl->handleNativeEvent (message);
                                                     }]==])

string(FIND "${_src}" "${_old}" _pos)
if(_pos EQUAL -1)
    message(FATAL_ERROR "PatchJuce.cmake: the expected code was not found in ${_file}. "
                        "JUCE was probably updated; the drag & drop patch needs to be adjusted.")
endif()

string(REPLACE "${_old}" "${_new}" _src "${_src}")
file(WRITE "${_file}" "${_src}")
message(STATUS "Applied FMX drag & drop patch to JUCE WebView2 backend")
