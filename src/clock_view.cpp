#include "clock_view.hpp"
#include <algorithm>
#include <array>
#include <new>

namespace ct {
namespace {
constexpr COLORREF Background = RGB(248, 250, 252);
constexpr std::array<COLORREF, 3> Ink{RGB(76, 92, 108), RGB(13, 100, 115), RGB(118, 128, 139)};
struct ClockView {
    std::array<HFONT, 3> fonts{};
    std::array<RECT, 3> regions{};
    std::array<std::wstring, 3> parts{L"00:00:", L"00", L".000"};
    UINT dpi{};
    bool compact{};
    HDC buffer{};
    HBITMAP bitmap{};
    HGDIOBJ originalBitmap{};
    int width{}, height{};
    std::optional<Ns> bucket;
    int offsetMinutes{};

    ~ClockView() {
        releaseBuffer();
        for (auto font : fonts)
            if (font)
                DeleteObject(font);
    }
    void releaseBuffer() {
        if (buffer && originalBitmap)
            SelectObject(buffer, originalBitmap);
        if (bitmap)
            DeleteObject(bitmap);
        if (buffer)
            DeleteDC(buffer);
        buffer = nullptr;
        bitmap = nullptr;
        originalBitmap = nullptr;
    }
    void style(HWND window, UINT nextDpi, bool nextCompact) {
        if (dpi == nextDpi && compact == nextCompact)
            return;
        dpi = nextDpi;
        compact = nextCompact;
        const std::array<int, 3> heights = compact ? std::array{28, 44, 15} : std::array{40, 60, 20};
        for (std::size_t i = 0; i < fonts.size(); ++i) {
            if (fonts[i])
                DeleteObject(fonts[i]);
            fonts[i] = CreateFontW(-MulDiv(heights[i], dpi, 96), 0, 0, 0, i == 1 ? FW_BOLD : FW_NORMAL, FALSE,
                                   FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
        }
        regions = {};
        InvalidateRect(window, nullptr, FALSE);
    }
    void setText(HWND window, const wchar_t* text) {
        const std::wstring next = text ? text : L"";
        if (next.size() != 12)
            return;
        const std::array<std::wstring, 3> split{next.substr(0, 6), next.substr(6, 2), next.substr(8, 4)};
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (parts[i] == split[i])
                continue;
            parts[i] = split[i];
            InvalidateRect(window, regions[i].right ? &regions[i] : nullptr, FALSE);
        }
    }
    bool render(HWND window, HDC destination, const RECT& dirty) {
        RECT client{};
        GetClientRect(window, &client);
        if (client.right <= 0 || client.bottom <= 0)
            return false;
        if (!buffer || width != client.right || height != client.bottom) {
            releaseBuffer();
            width = client.right;
            height = client.bottom;
            buffer = CreateCompatibleDC(destination);
            bitmap = CreateCompatibleBitmap(destination, width, height);
            if (!buffer || !bitmap) {
                releaseBuffer();
                return false;
            }
            originalBitmap = SelectObject(buffer, bitmap);
        }
        // Compose a complete opaque frame in memory. WM_ERASEBKGND never exposes a blank frame.
        const auto saved = SaveDC(buffer);
        SetDCBrushColor(buffer, Background);
        FillRect(buffer, &client, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        SetBkMode(buffer, TRANSPARENT);
        SetTextAlign(buffer, TA_LEFT | TA_BASELINE);
        std::array<int, 3> advances{};
        int ascent = 0, descent = 0;
        for (std::size_t i = 0; i < fonts.size(); ++i) {
            SelectObject(buffer, fonts[i]);
            TEXTMETRICW metrics{};
            GetTextMetricsW(buffer, &metrics);
            SIZE size{};
            GetTextExtentPoint32W(buffer, parts[i].c_str(), static_cast<int>(parts[i].size()), &size);
            advances[i] = size.cx;
            ascent = std::max(ascent, static_cast<int>(metrics.tmAscent));
            descent = std::max(descent, static_cast<int>(metrics.tmDescent));
        }
        const int baseline = std::max(0, (height - ascent - descent) / 2) + ascent;
        int x = 0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const int right = i == 2 ? width : x + advances[i] + MulDiv(3, dpi, 96);
            regions[i] = {x, 0, right, height};
            SelectObject(buffer, fonts[i]);
            SetTextColor(buffer, Ink[i]);
            ExtTextOutW(buffer, x, baseline, ETO_CLIPPED, &regions[i], parts[i].c_str(),
                        static_cast<UINT>(parts[i].size()), nullptr);
            x = right;
        }
        RestoreDC(buffer, saved);
        // A fraction-only update copies only its region; the seconds remain untouched on screen.
        return BitBlt(destination, dirty.left, dirty.top, dirty.right - dirty.left, dirty.bottom - dirty.top,
                      buffer, dirty.left, dirty.top, SRCCOPY) != FALSE;
    }
};
ClockView* view(HWND window) {
    return reinterpret_cast<ClockView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}
LRESULT CALLBACK clockProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto state = view(window);
    if (message == WM_NCCREATE) {
        state = new (std::nothrow) ClockView;
        if (!state)
            return FALSE;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state) {
        switch (message) {
        case WM_CREATE:
            state->style(window, GetDpiForWindow(window), false);
            return 0;
        case WM_SETTEXT:
            state->setText(window, reinterpret_cast<const wchar_t*>(lp));
            // Keep the caption available to accessibility, without STATIC's erase/repaint path.
            return DefWindowProcW(window, message, wp, lp);
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            state->regions = {};
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            state->render(window, dc, paint.rcPaint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINTCLIENT: {
            RECT rect{};
            GetClientRect(window, &rect);
            state->render(window, reinterpret_cast<HDC>(wp), rect);
            return 0;
        }
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            delete state;
            break;
        }
    }
    return DefWindowProcW(window, message, wp, lp);
}
} // namespace
bool registerClockView(HINSTANCE instance) {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = ClockViewClass;
    cls.lpfnWndProc = clockProc;
    // No class background brush: every visible update is an already-composed frame.
    return RegisterClassW(&cls) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}
void styleClockView(HWND window, UINT dpi, bool compact) {
    if (auto state = view(window))
        state->style(window, dpi, compact);
}
void updateClockView(HWND window, Ns utc, int utcOffsetMinutes) {
    auto state = view(window);
    if (!state)
        return;
    const Ns bucket = utc / (100 * Millisecond);
    if (state->bucket == bucket && state->offsetMinutes == utcOffsetMinutes)
        return;
    state->bucket = bucket;
    state->offsetMinutes = utcOffsetMinutes;
    SetWindowTextW(window, formatTime(utc, false, utcOffsetMinutes).c_str());
}
} // namespace ct
