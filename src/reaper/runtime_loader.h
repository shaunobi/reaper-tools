#pragma once
// Windows: makes sure the ONNX Runtime and DirectML libraries that ship next
// to reaper_srt.dll are the ones the process uses. Windows itself installs
// older copies of both in System32 for its own machine-learning features,
// and the default search order finds those before UserPlugins. An older
// runtime returns a null API table for the version we were built against,
// which crashes on the first call.
//
// onnxruntime.dll is delay-loaded and resolved through a delay-load hook to
// the module we load here by full path. DirectML.dll is loaded by full path
// before the runtime asks for it by name, so the by-name lookup finds ours.

#include <string>
#include <vector>

namespace srt::reaper {

// Call once from the plugin entry point before any runtime use. Returns
// true when the runtime next to the extension is usable; on failure the
// returned lines explain what was found.
bool loadRuntimeLibraries(void* hInstance, std::vector<std::string>& report);

// Human-readable lines describing the runtime modules actually loaded in
// the process (paths and versions), for the About action.
std::vector<std::string> runtimeModuleReport();

} // namespace srt::reaper
