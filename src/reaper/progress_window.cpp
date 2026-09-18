#include "progress_window.h"

#include "host.h"
#include "resource.h"
#include "wdltypes.h"

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

constexpr int kRowHeight = 44;
constexpr int kMargin = 10;
constexpr int kButtonAreaHeight = 34;
constexpr int kCancelBoxSize = 16;

HWND g_hwnd = nullptr;
srt::JobRunner* g_runner = nullptr;
std::vector<srt::JobStatus> g_rows;

RECT rowRect(const RECT& client, size_t row) {
    RECT r;
    r.left = client.left + kMargin;
    r.right = client.right - kMargin;
    r.top = client.top + kMargin + static_cast<LONG>(row) * kRowHeight;
    r.bottom = r.top + kRowHeight;
    return r;
}

RECT cancelBoxRect(const RECT& row) {
    RECT r;
    r.right = row.right;
    r.left = r.right - kCancelBoxSize;
    r.top = row.top + 2;
    r.bottom = r.top + kCancelBoxSize;
    return r;
}

void fill(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);

    const COLORREF bg = RGB(240, 240, 240);
    const COLORREF text = RGB(20, 20, 20);
    const COLORREF barBg = RGB(200, 200, 200);
    const COLORREF barFg = RGB(70, 130, 220);
    fill(dc, client, bg);

    HFONT font = reinterpret_cast<HFONT>(SendMessage(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);

    for (size_t i = 0; i < g_rows.size(); ++i) {
        const srt::JobStatus& st = g_rows[i];
        RECT row = rowRect(client, i);

        char label[512];
        std::snprintf(label, sizeof(label), "%s  %3d%%  %s", st.name.c_str(),
                      static_cast<int>(st.fraction * 100.0 + 0.5), st.message.c_str());
        RECT textRect = row;
        textRect.bottom = row.top + 18;
        textRect.right -= kCancelBoxSize + 6;
        DrawText(dc, label, -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        RECT bar = row;
        bar.top = row.top + 22;
        bar.bottom = bar.top + 12;
        bar.right -= kCancelBoxSize + 6;
        fill(dc, bar, barBg);
        RECT done = bar;
        done.right = bar.left + static_cast<LONG>((bar.right - bar.left) * std::clamp(st.fraction, 0.0, 1.0));
        fill(dc, done, barFg);

        RECT box = cancelBoxRect(row);
        fill(dc, box, RGB(180, 60, 60));
        SetTextColor(dc, RGB(255, 255, 255));
        DrawText(dc, "x", -1, &box, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, text);
    }
    if (oldFont) SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

void layout(HWND hwnd) {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    RECT cr;
    GetClientRect(hwnd, &cr);
    const int frameH = (wr.bottom - wr.top) - (cr.bottom - cr.top);
    const int rows = static_cast<int>(std::max<size_t>(1, g_rows.size()));
    const int wantClientH = kMargin + rows * kRowHeight + kButtonAreaHeight;
    SetWindowPos(hwnd, nullptr, 0, 0, wr.right - wr.left, wantClientH + frameH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    GetClientRect(hwnd, &cr);
    HWND btn = GetDlgItem(hwnd, IDC_SRT_CANCEL_ALL);
    if (btn) {
        RECT br;
        GetWindowRect(btn, &br);
        const int bw = br.right - br.left, bh = br.bottom - br.top;
        SetWindowPos(btn, nullptr, cr.right - kMargin - bw, cr.bottom - kMargin - bh, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

WDL_DLGRET dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 1;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN: {
        RECT client;
        GetClientRect(hwnd, &client);
        POINT pt = {static_cast<LONG>(static_cast<short>(LOWORD(lParam))),
                    static_cast<LONG>(static_cast<short>(HIWORD(lParam)))};
        for (size_t i = 0; i < g_rows.size(); ++i) {
            RECT box = cancelBoxRect(rowRect(client, i));
            if (PtInRect(&box, pt) && g_runner) g_runner->cancel(g_rows[i].id);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_SRT_CANCEL_ALL && g_runner) g_runner->cancelAll();
        else if (LOWORD(wParam) == IDCANCEL) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_hwnd = nullptr;
        return 0;
    }
    return 0;
}

} // namespace

void progressWindowRefresh(srt::JobRunner& runner) {
    g_runner = &runner;
    std::vector<srt::JobStatus> rows = runner.statuses();
    const bool hadRows = !g_rows.empty();
    const size_t oldCount = g_rows.size();
    g_rows = std::move(rows);

    if (g_rows.empty()) {
        if (g_hwnd && hadRows) ShowWindow(g_hwnd, SW_HIDE);
        return;
    }
    if (!g_hwnd) {
        g_hwnd = CreateDialogParam(g_hInstance, MAKEINTRESOURCE(IDD_SRT_PROGRESS), GetMainHwnd(), dialogProc, 0);
        if (!g_hwnd) return;
        layout(g_hwnd);
        ShowWindow(g_hwnd, SW_SHOWNA);
        return;
    }
    if (!hadRows) {
        layout(g_hwnd);
        ShowWindow(g_hwnd, SW_SHOWNA);
    } else if (oldCount != g_rows.size()) {
        layout(g_hwnd);
    } else {
        InvalidateRect(g_hwnd, nullptr, FALSE);
    }
}

void progressWindowDestroy() {
    if (g_hwnd) DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
    g_runner = nullptr;
    g_rows.clear();
}

} // namespace srt::reaper
