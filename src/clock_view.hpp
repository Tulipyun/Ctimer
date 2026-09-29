#pragma once
#include "platform.hpp"

namespace ct {
inline constexpr wchar_t ClockViewClass[] = L"CtimerClockView";
bool registerClockView(HINSTANCE instance);
void styleClockView(HWND window, UINT dpi, bool compact);
// Presentation only: sample the displayed fraction at 10 Hz. Never rounds the engine clock.
void updateClockView(HWND window, Ns utc, int utcOffsetMinutes);
} // namespace ct
