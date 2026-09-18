// Non-modal "SRT jobs" window built entirely from stock dialog controls
// (static text, msctls_progress32, push buttons), the same way SWS builds
// its dialogs, so it renders natively on Windows and through SWELL on macOS
// and Linux with no custom drawing.

#include "progress_window.h"

#include "host.h"
#include "resource.h"
#include "wdltypes.h"

#ifdef _WIN32
#include <commctrl.h> // PBM_* messages for msctls_progress32
#endif

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifndef _WIN32
#include "swell/swell-dlggen.h"
#include "resource.rc_mac_dlg"
#endif

namespace srt::reaper {

namespace {

constexpr int kBarRange = 1000;

HWND g_hwnd = nullptr;
srt::JobRunner* g_runner = nullptr;
std::vector<srt::JobStatus> g_rows;
size_t g_shownRows = 0;
std::vector<std::string> g_lastLabels(SRT_PROGRESS_ROWS);

void showRow(int row, bool show) {
    const int cmd = show ? SW_SHOW : SW_HIDE;
    ShowWindow(GetDlgItem(g_hwnd, IDC_SRT_LABEL0 + row), cmd);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SRT_BAR0 + row), cmd);
    ShowWindow(GetDlgItem(g_hwnd, IDC_SRT_CANCEL0 + row), cmd);
}

// Shrinks the window so it ends just below the last visible row (or the
// "more" line), and parks the Cancel all button at the bottom.
void layout(size_t rows) {
    RECT win, client;
    GetWindowRect(g_hwnd, &win);
    GetClientRect(g_hwnd, &client);
    const int frameH = (win.bottom - win.top) - (client.bottom - client.top);

    // Bottom edge of the last used row, in client coordinates.
    const int lastRow = static_cast<int>(std::max<size_t>(1, rows)) - 1;
    RECT anchor;
    GetWindowRect(GetDlgItem(g_hwnd, IDC_SRT_CANCEL0 + lastRow), &anchor);
    POINT origin = {0, 0};
    ClientToScreen(g_hwnd, &origin);
    const int bottom = anchor.bottom - origin.y;

    HWND more = GetDlgItem(g_hwnd, IDC_SRT_MORE);
    HWND cancelAll = GetDlgItem(g_hwnd, IDC_SRT_CANCEL_ALL);
    RECT moreRect, cancelRect;
    GetWindowRect(more, &moreRect);
    GetWindowRect(cancelAll, &cancelRect);
    const int moreH = moreRect.bottom - moreRect.top;
    const int cancelW = cancelRect.right - cancelRect.left, cancelH = cancelRect.bottom - cancelRect.top;
    const int gap = 8;

    int y = bottom + gap;
    const bool showMore = g_rows.size() > static_cast<size_t>(SRT_PROGRESS_ROWS);
    if (showMore) {
        SetWindowPos(more, nullptr, moreRect.left - origin.x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        y += moreH + gap;
    }
    ShowWindow(more, showMore ? SW_SHOW : SW_HIDE);
    SetWindowPos(cancelAll, nullptr, cancelRect.left - origin.x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    y += cancelH + gap;
    (void)cancelW;

    SetWindowPos(g_hwnd, nullptr, 0, 0, win.right - win.left, y + frameH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

WDL_DLGRET dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG:
        for (int i = 0; i < SRT_PROGRESS_ROWS; ++i) {
            SendMessage(GetDlgItem(hwnd, IDC_SRT_BAR0 + i), PBM_SETRANGE, 0, MAKELPARAM(0, kBarRange));
            SendMessage(GetDlgItem(hwnd, IDC_SRT_BAR0 + i), PBM_SETPOS, 0, 0);
        }
        return 1;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_SRT_CANCEL_ALL && g_runner) g_runner->cancelAll();
        else if (id == IDCANCEL) ShowWindow(hwnd, SW_HIDE);
        else if (id >= IDC_SRT_CANCEL0 && id < IDC_SRT_CANCEL0 + SRT_PROGRESS_ROWS && g_runner) {
            const size_t row = static_cast<size_t>(id - IDC_SRT_CANCEL0);
            if (row < g_rows.size()) g_runner->cancel(g_rows[row].id);
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

void updateControls() {
    const size_t rows = std::min<size_t>(g_rows.size(), SRT_PROGRESS_ROWS);
    for (size_t i = 0; i < static_cast<size_t>(SRT_PROGRESS_ROWS); ++i) {
        if (i < rows) {
            const srt::JobStatus& st = g_rows[i];
            char label[512];
            std::snprintf(label, sizeof(label), "%s  %d%%  %s", st.name.c_str(),
                          static_cast<int>(st.fraction * 100.0 + 0.5), st.message.c_str());
            if (g_lastLabels[i] != label) {
                SetDlgItemText(g_hwnd, IDC_SRT_LABEL0 + static_cast<int>(i), label);
                g_lastLabels[i] = label;
            }
            SendMessage(GetDlgItem(g_hwnd, IDC_SRT_BAR0 + static_cast<int>(i)), PBM_SETPOS,
                        static_cast<WPARAM>(std::clamp(st.fraction, 0.0, 1.0) * kBarRange), 0);
        } else if (!g_lastLabels[i].empty()) {
            g_lastLabels[i].clear();
        }
    }
    if (g_rows.size() > static_cast<size_t>(SRT_PROGRESS_ROWS)) {
        char more[64];
        std::snprintf(more, sizeof(more), "and %zu more queued", g_rows.size() - SRT_PROGRESS_ROWS);
        SetDlgItemText(g_hwnd, IDC_SRT_MORE, more);
    }
    if (rows != g_shownRows) {
        for (int i = 0; i < SRT_PROGRESS_ROWS; ++i) showRow(i, static_cast<size_t>(i) < rows);
        g_shownRows = rows;
        layout(rows);
    }
}

} // namespace

void progressWindowRefresh(srt::JobRunner& runner) {
    g_runner = &runner;
    const bool hadRows = !g_rows.empty();
    g_rows = runner.statuses();

    if (g_rows.empty()) {
        if (g_hwnd && hadRows) {
            ShowWindow(g_hwnd, SW_HIDE);
            g_shownRows = 0;
        }
        return;
    }
    if (!g_hwnd) {
        g_hwnd = CreateDialogParam(g_hInstance, MAKEINTRESOURCE(IDD_SRT_PROGRESS), GetMainHwnd(), dialogProc, 0);
        if (!g_hwnd) return;
        g_shownRows = static_cast<size_t>(-1); // force a layout pass
    }
    updateControls();
    if (!hadRows) ShowWindow(g_hwnd, SW_SHOWNA);
}

void progressWindowDestroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_runner = nullptr;
    g_rows.clear();
    g_shownRows = 0;
}

} // namespace srt::reaper
