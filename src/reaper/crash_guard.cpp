#include "crash_guard.h"

// Structured exception handling (__try/__except) needs the Microsoft
// compiler or clang (with -fms-extensions, see CMakeLists.txt); GCC gets the
// plain version.
#if defined(_WIN32) && (defined(_MSC_VER) || defined(__clang__))
#include <windows.h>
#include <cstdio>

namespace srt::reaper {

namespace {

struct Call {
    const std::function<void()>* fn;
};

void invoke(Call* c) { (*c->fn)(); }

// No C++ objects with destructors may live in a function that uses __try.
int guardedInvoke(Call* c, DWORD* code, void** address) {
    __try {
        invoke(c);
        return 1;
    } __except (*code = GetExceptionCode(),
                *address = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
                EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

const char* describe(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    default: return "hardware exception";
    }
}

} // namespace

bool runGuarded(const std::function<void()>& fn, std::string& error) {
    Call c{&fn};
    DWORD code = 0;
    void* address = nullptr;
    if (guardedInvoke(&c, &code, &address)) return true;

    char moduleName[MAX_PATH] = "unknown module";
    HMODULE m = nullptr;
    if (address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      static_cast<LPCWSTR>(address), &m) && m) {
        wchar_t path[MAX_PATH];
        if (GetModuleFileNameW(m, path, MAX_PATH)) {
            const wchar_t* base = wcsrchr(path, L'\\');
            base = base ? base + 1 : path;
            WideCharToMultiByte(CP_UTF8, 0, base, -1, moduleName, sizeof(moduleName), nullptr, nullptr);
        }
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s (code 0x%08lX) at %p in %s", describe(code), static_cast<unsigned long>(code),
                  address, moduleName);
    error = buf;
    return false;
}

} // namespace srt::reaper

#else

namespace srt::reaper {
bool runGuarded(const std::function<void()>& fn, std::string&) {
    fn();
    return true;
}
} // namespace srt::reaper

#endif
