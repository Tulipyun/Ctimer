#include "clock_view.hpp"
#include <algorithm>
#include <iostream>
#include <vector>

void check(bool condition, const char* label);
namespace {
struct Canvas {
    HDC dc{CreateCompatibleDC(nullptr)};
    HBITMAP bitmap{};
    HGDIOBJ original{};
    std::uint32_t* pixels{};
    int width, height;
    Canvas(int w, int h) : width(w), height(h) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = w;
        info.bmiHeader.biHeight = -h;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
        original = SelectObject(dc, bitmap);
    }
    ~Canvas() {
        SelectObject(dc, original);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    std::vector<std::uint32_t> read(HWND window) {
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        GdiFlush();
        return {pixels, pixels + width * height};
    }
};
} // namespace
void runClockViewTests() {
    using namespace ct;
    check(registerClockView(GetModuleHandleW(nullptr)), "register isolated clock renderer");
    // Hidden windows have no update region. Use an off-screen, non-activating window
    // to exercise real invalidation without covering the desktop or taking focus.
    auto window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClockViewClass, L"00:00:00.000",
                                  WS_POPUP | WS_VISIBLE, -30000, -30000, 366, 76, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    check(window != nullptr, "create off-screen clock rendering test without focus input or network");
    if (!window)
        return;
    constexpr Ns start = ((11 * 60 + 2) * 60 + 28LL) * Second + 221 * Millisecond;
    {
        Canvas canvas(366, 76);
        styleClockView(window, 96, false);
        updateClockView(window, start, 0);
        auto first = canvas.read(window);
        ValidateRect(window, nullptr);
        updateClockView(window, start + 60 * Millisecond, 0);
        RECT dirty{};
        check(!GetUpdateRect(window, &dirty, FALSE),
              "display sampling does not repaint within a 100ms bucket");
        updateClockView(window, start + 100 * Millisecond, 0);
        const bool invalid = GetUpdateRect(window, &dirty, FALSE) != FALSE;
        if (!invalid || dirty.left <= 366 / 2)
            std::cerr << "Clock invalid region: " << invalid << " [" << dirty.left << ',' << dirty.top << ','
                      << dirty.right << ',' << dirty.bottom << "]\n";
        check(invalid && dirty.left > 366 / 2, "fraction update invalidates only the right-hand region");
        auto second = canvas.read(window);
        bool prefixUnchanged = true, fractionChanged = false;
        for (int y = 0; y < canvas.height; ++y)
            for (int x = 0; x < canvas.width; ++x) {
                auto i = y * canvas.width + x;
                if (x < dirty.left)
                    prefixUnchanged = prefixUnchanged && first[i] == second[i];
                else
                    fractionChanged = fractionChanged || first[i] != second[i];
            }
        check(prefixUnchanged && fractionChanged,
              "millisecond redraw preserves hour minute and second pixels");
        SendMessageW(window, WM_ERASEBKGND, reinterpret_cast<WPARAM>(canvas.dc), 0);
        GdiFlush();
        check(std::equal(second.begin(), second.end(), canvas.pixels),
              "background erase never blanks an existing clock frame");
        updateClockView(window, start + 800 * Millisecond, 0);
        wchar_t caption[32]{};
        GetWindowTextW(window, caption, 32);
        check(std::wstring(caption) == L"11:02:29.021",
              "second rollover retains a coherent sampled timestamp for accessibility");
        updateClockView(window, start - 300 * Millisecond, 0);
        GetWindowTextW(window, caption, 32);
        check(std::wstring(caption) == L"11:02:27.921", "display tolerates backwards clock correction");
    }
    bool visible = true, unclipped = true, stableHandles = true;
    for (UINT dpi : {96u, 120u, 144u, 192u}) {
        for (bool compact : {false, true}) {
            const int width = MulDiv(compact ? 264 : 366, dpi, 96);
            const int height = MulDiv(compact ? 62 : 76, dpi, 96);
            SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            styleClockView(window, dpi, compact);
            Canvas canvas(width, height);
            auto frame = canvas.read(window);
            const auto handles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (int i = 0; i < 200; ++i) {
                updateClockView(window, start + static_cast<Ns>(i) * 100 * Millisecond, 0);
                frame = canvas.read(window);
                int ink = 0;
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x)
                        if ((frame[y * width + x] & 0xffffff) != 0xf8fafc) {
                            ++ink;
                            if (y == 0 || y == height - 1 || x == width - 1)
                                unclipped = false;
                        }
                visible = visible && ink > 300;
            }
            stableHandles = stableHandles && GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= handles;
        }
    }
    check(visible && unclipped,
          "1600 rendered frames retain visible unclipped digits across both layouts and four DPI scales");
    check(stableHandles, "continuous clock painting reuses GDI resources without growth");
    DestroyWindow(window);
}
