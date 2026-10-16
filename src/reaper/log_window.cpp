// The SRT log window: a read-only multiline edit control in a resizable
// dialog, built from stock controls so it works on Windows and through
// SWELL on macOS and Linux.

#include "log_window.h"

#include "host.h"
#include "resource.h"
#include "wdltypes.h"

#include <string>
#include <vector>

#ifndef _WIN32
#include "swell/swell-dlggen.h"
#include "resource.rc_mac_dlg"
#endif

namespace srt::reaper {

namespace {

HWND g_hwnd = nullptr;

#ifdef _WIN32
const char* kNewline = "\r\n";
#else
const char* kNewline = "\n";
#endif

// Rewrites the whole text from the history and scrolls to the end. Lines
// arrive a few at a time, so replacing the text is simpler than appending
// and needs nothing beyond what SWELL implements.
void fillFromHistory() {
    std::string all;
    for (const auto& l : logHistory()) all += l + kNewline;
    HWND edit = GetDlgItem(g_hwnd, IDC_SRT_LOG_TEXT);
    SetWindowText(edit, all.c_str());
    const auto len = static_cast<WPARAM>(all.size());
    SendMessage(edit, EM_SETSEL, len, static_cast<LPARAM>(len));
#ifdef EM_SCROLLCARET // Windows only; SWELL scrolls to the selection by itself
    SendMessage(edit, EM_SCROLLCARET, 0, 0);
#endif
}

void layout(HWND hwnd) {
    RECT c;
    GetClientRect(hwnd, &c);
    const int margin = 6, bottomRow = 18, gap = 6;
    HWND edit = GetDlgItem(hwnd, IDC_SRT_LOG_TEXT);
    HWND path = GetDlgItem(hwnd, IDC_SRT_LOG_PATH);
    HWND clear = GetDlgItem(hwnd, IDC_SRT_LOG_CLEAR);
    RECT clearRect, pathRect;
    GetWindowRect(clear, &clearRect);
    GetWindowRect(path, &pathRect);
    const int clearW = clearRect.right - clearRect.left, clearH = clearRect.bottom - clearRect.top;
    const int pathH = pathRect.bottom - pathRect.top;
    const int width = c.right - c.left, height = c.bottom - c.top;
    const int rowY = height - margin - bottomRow;
    SetWindowPos(edit, nullptr, margin, margin, width - 2 * margin, rowY - gap - margin, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(path, nullptr, margin, rowY + (bottomRow - pathH) / 2, width - margin - clearW - 2 * gap, pathH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(clear, nullptr, width - margin - clearW, rowY + (bottomRow - clearH) / 2, clearW, clearH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

WDL_DLGRET dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        g_hwnd = hwnd;
        SetDlgItemText(hwnd, IDC_SRT_LOG_PATH, logFilePath().c_str());
        fillFromHistory();
        layout(hwnd);
        return 1;
    case WM_SIZE:
        layout(hwnd);
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_SRT_LOG_CLEAR) {
            clearLogHistory();
            SetDlgItemText(hwnd, IDC_SRT_LOG_TEXT, "");
        } else if (id == IDCANCEL) {
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    }
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_hwnd = nullptr;
        return 0;
    }
    return 0;
}

bool ensureWindow() {
    if (g_hwnd) return true;
    g_hwnd = CreateDialogParam(g_hInstance, MAKEINTRESOURCE(IDD_SRT_LOG), GetMainHwnd(), dialogProc, 0);
    return g_hwnd != nullptr;
}

} // namespace

void logWindowAppend(const std::string&) {
    if (g_hwnd && IsWindowVisible(g_hwnd)) fillFromHistory();
}

void logWindowShow() {
    if (!ensureWindow()) return;
    fillFromHistory(); // catches up on lines logged while hidden
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
}

void logWindowToggle() {
    if (g_hwnd && IsWindowVisible(g_hwnd)) {
        ShowWindow(g_hwnd, SW_HIDE);
        return;
    }
    logWindowShow();
}

void logWindowDestroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}

} // namespace srt::reaper
