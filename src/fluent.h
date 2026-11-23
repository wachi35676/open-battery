#pragma once
// Windows 11 (Fluent) drawing shared by the click panel and the settings window: theme colours, the
// type ramp, Segoe Fluent Icons, and the standard controls (cards, buttons, toggle switches, combo boxes),
// drawn with Direct2D on a Mica / Acrylic system backdrop. Sizes follow the Settings app and Quick
// Settings measured at 100% scale.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include "devices.h"

namespace fluent {

// Theme resources (dark and light), as WinUI names them.
struct Palette {
    D2D1_COLOR_F text, textSecondary, textTertiary, textDisabled;
    D2D1_COLOR_F layer, card, cardSecondary, cardHover, cardPressed, cardStroke;
    D2D1_COLOR_F control, controlHover, controlPressed, controlStroke, controlStrongStroke;
    D2D1_COLOR_F subtleHover, subtlePressed, divider;
    D2D1_COLOR_F accent, accentHover, accentPressed, onAccent;
    D2D1_COLOR_F popup, popupStroke, critical, success;
};
Palette Colors(bool light);

// Icon code points used by more than one window.
namespace glyph {
constexpr wchar_t Settings = 0xE713, Refresh = 0xE72C, ChevronRight = 0xE76C, ChevronDown = 0xE70D, ChevronUp = 0xE70E,
                  History = 0xE81C, Devices = 0xE772, Bolt = 0xE945, Check = 0xE73E, BatteryUnknown = 0xE996;
}
wchar_t DeviceGlyph(DeviceKind kind, bool bluetooth);
wchar_t BatteryGlyph(int percent, bool charging);  // Windows' Battery0..10 / BatteryCharging0..10

// Type ramp sizes.
constexpr float kCaption = 12, kBody = 14, kSubtitle = 20, kTitle = 28;

// Draws into one window. Work in DIPs; the painter scales to the window's DPI.
class Painter {
public:
    float scale = 1;  // pixels per DIP
    float alpha = 1;  // multiplies every colour (fades)
    bool Begin(HWND wnd);  // false if drawing is not possible right now
    void End();
    void Resize(UINT width, UINT height);
    void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1);
    void Rect(D2D1_RECT_F r, D2D1_COLOR_F c);
    void Circle(float cx, float cy, float radius, D2D1_COLOR_F c);
    // One line (or a wrapped block) in `r`, vertically centred; align 0 left, 1 centre, 2 right.
    void Text(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, D2D1_RECT_F r, D2D1_COLOR_F c,
              int align = 0, bool wrap = false);
    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight);
    void Glyph(wchar_t g, float size, D2D1_RECT_F r, D2D1_COLOR_F c, float rotation = 0);
    void Clip(D2D1_RECT_F r);
    void Unclip();
    ID2D1HwndRenderTarget* Target() const { return rt_; }

private:
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* brush_ = nullptr;
    HWND wnd_ = nullptr;
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c);
};

// Interaction state of a control: resting, hovered, pressed, disabled.
enum class Look { Rest, Hover, Press, Disabled };

// Standard controls (WinUI sizes).
void ToggleSwitch(Painter& p, const Palette& c, float right, float cy, bool on, Look look);  // 40 x 20, right-aligned
void Button(Painter& p, const Palette& c, D2D1_RECT_F r, const std::wstring& label, Look look, bool accent = false);
void ComboBox(Painter& p, const Palette& c, D2D1_RECT_F r, const std::wstring& value, Look look);
float ComboWidth(Painter& p, const std::wstring& longest);  // fits the longest option, 120 at least
void IconButton(Painter& p, const Palette& c, D2D1_RECT_F r, wchar_t g, float glyphSize, Look look, float rotation = 0);

// Window backdrops. Mica and Acrylic are the system materials (their opacity is fixed by Windows);
// Glass is a lighter blur of what is behind the window with a thin tint, for more transparency.
enum class Backdrop { Mica = 0, Acrylic = 1, Glass = 2 };
// DWM shows Mica's plain grey fallback instead of the wallpaper-tinted material unless this runs with an
// activating frame change both before the window is shown and again after it is up.
void ApplyBackdrop(HWND wnd, bool light, Backdrop backdrop);
bool BackdropSupported(HWND wnd);  // Windows 11 22H2 and later
UINT WindowDpi(HWND wnd);
bool AnimationsEnabled();
IDWriteFactory* DWrite();

}  // namespace fluent
