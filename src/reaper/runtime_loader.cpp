#include "runtime_loader.h"

#include <onnxruntime_c_api.h>

#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <delayimp.h>
#include <filesystem>
#include <sstream>

namespace srt::reaper {

namespace {

HMODULE g_ort = nullptr;
HMODULE g_dml = nullptr;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring modulePath(HMODULE m) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(m, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    return n ? std::wstring(buf, n) : std::wstring();
}

std::string fileVersion(const std::wstring& path) {
    // Reads the "FileVersion" resource without linking version.lib at build
    // time: resolve the three functions dynamically.
    HMODULE ver = LoadLibraryW(L"version.dll");
    if (!ver) return "";
    using GetSizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using GetInfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    auto getSize = reinterpret_cast<GetSizeFn>(GetProcAddress(ver, "GetFileVersionInfoSizeW"));
    auto getInfo = reinterpret_cast<GetInfoFn>(GetProcAddress(ver, "GetFileVersionInfoW"));
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(ver, "VerQueryValueW"));
    std::string out;
    if (getSize && getInfo && query) {
        DWORD handle = 0;
        const DWORD size = getSize(path.c_str(), &handle);
        if (size) {
            std::vector<char> data(size);
            VS_FIXEDFILEINFO* info = nullptr;
            UINT len = 0;
            if (getInfo(path.c_str(), 0, size, data.data()) &&
                query(data.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &len) && info) {
                std::ostringstream s;
                s << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.'
                  << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
                out = s.str();
            }
        }
    }
    FreeLibrary(ver);
    return out;
}

// Delay-load hook: hand the delay-load helper our module for onnxruntime.dll
// instead of letting it search by name.
FARPROC WINAPI delayHook(unsigned reason, DelayLoadInfo* info) {
    if (reason == dliNotePreLoadLibrary && info && info->szDll && g_ort && _stricmp(info->szDll, "onnxruntime.dll") == 0)
        return reinterpret_cast<FARPROC>(g_ort);
    return nullptr;
}

} // namespace

} // namespace srt::reaper

// The delay-load helper looks for this symbol. Microsoft's delayimp.h declares
// it const (Visual Studio 2015 and later); MinGW's does not.
extern "C"
#ifdef _MSC_VER
const
#endif
PfnDliHook __pfnDliNotifyHook2 = srt::reaper::delayHook;

namespace srt::reaper {

bool loadRuntimeLibraries(void* hInstance, std::vector<std::string>& report) {
    const std::wstring self = modulePath(static_cast<HMODULE>(hInstance));
    if (self.empty()) {
        report.push_back("cannot determine the extension's own path");
        return false;
    }
    const std::filesystem::path dir = std::filesystem::path(self).parent_path();

    const std::filesystem::path dmlPath = dir / L"DirectML.dll";
    g_dml = LoadLibraryExW(dmlPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_dml) report.push_back("could not load " + narrow(dmlPath.wstring()) + " (error " + std::to_string(GetLastError()) + "); DirectML will be unavailable");

    const std::filesystem::path ortPath = dir / L"onnxruntime.dll";
    g_ort = LoadLibraryExW(ortPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_ort) {
        report.push_back("could not load " + narrow(ortPath.wstring()) + " (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    // First runtime call: the delay-load hook routes it to g_ort.
    const OrtApiBase* base = OrtGetApiBase();
    if (!base || !base->GetApi(ORT_API_VERSION)) {
        report.push_back(std::string("the loaded onnxruntime.dll does not provide API version ") + std::to_string(ORT_API_VERSION) +
                         " (it reports version " + (base ? base->GetVersionString() : "unknown") + ")");
        return false;
    }
    return true;
}

std::vector<std::string> runtimeModuleReport() {
    std::vector<std::string> out;
    for (const wchar_t* name : {L"onnxruntime.dll", L"DirectML.dll"}) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) {
            out.push_back(narrow(name) + ": not loaded");
            continue;
        }
        const std::wstring p = modulePath(m);
        const std::string ver = fileVersion(p);
        out.push_back(narrow(name) + ": " + narrow(p) + (ver.empty() ? "" : " (file version " + ver + ")"));
    }
    return out;
}

} // namespace srt::reaper

#else // not Windows

namespace srt::reaper {

bool loadRuntimeLibraries(void*, std::vector<std::string>&) {
    const OrtApiBase* base = OrtGetApiBase();
    return base && base->GetApi(ORT_API_VERSION);
}

std::vector<std::string> runtimeModuleReport() {
    const OrtApiBase* base = OrtGetApiBase();
    return {std::string("onnxruntime: version ") + (base ? base->GetVersionString() : "unknown") +
            ", loaded from the library next to the extension"};
}

} // namespace srt::reaper

#endif
