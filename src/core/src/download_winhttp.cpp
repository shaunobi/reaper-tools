#include "srt/download.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace srt {

bool downloadSupported() { return true; }

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

std::string lastError(const char* what) {
    return std::string(what) + " (WinHTTP error " + std::to_string(GetLastError()) + ")";
}

} // namespace

void downloadToStream(const std::string& url, std::ofstream& out,
                      const std::function<void(uint64_t, uint64_t)>& onBytes, const std::function<bool()>& cancelled) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {0}, path[2048] = {0};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) throw std::runtime_error("invalid URL: " + url);

    Handle session;
    session.h = WinHttpOpen(L"reaper-srt/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) throw std::runtime_error(lastError("cannot open HTTP session"));
    Handle connect;
    connect.h = WinHttpConnect(session.h, host, parts.nPort, 0);
    if (!connect.h) throw std::runtime_error(lastError("cannot connect"));
    Handle request;
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) throw std::runtime_error(lastError("cannot open request"));
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        throw std::runtime_error(lastError("request failed"));

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) throw std::runtime_error("server answered HTTP " + std::to_string(status) + " for " + url);
    wchar_t lenBuf[32] = {0};
    DWORD lenSize = sizeof(lenBuf);
    uint64_t total = 0;
    if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize,
                            WINHTTP_NO_HEADER_INDEX))
        total = static_cast<uint64_t>(_wtoi64(lenBuf));

    std::vector<char> buf(1 << 16);
    uint64_t received = 0;
    for (;;) {
        if (cancelled()) throw JobCancelled{};
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got))
            throw std::runtime_error(lastError("read failed"));
        if (got == 0) break;
        out.write(buf.data(), got);
        if (!out) throw std::runtime_error("write failed while downloading");
        received += got;
        onBytes(received, total);
    }
    out.flush();
}

} // namespace srt
