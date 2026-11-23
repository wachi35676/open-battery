#include "flyout.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

namespace {

// DWM attributes, defined here because older SDK / MinGW headers lack them.
constexpr DWORD kDwmDarkMode = 20;    // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmCorners = 33;     // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBackdrop = 38;    // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kBackdropMica = 2, kBackdropAcrylic = 3, kCornersRound = 2;

// Layout in DIPs, measured from Windows 11's Quick Settings flyout at 100% scale: 360 wide, 24 px side
// and 23 px top padding, 96 x 48 tiles 12 px apart with the label in the 48 px under each, 16 px icons,
// 4 px control corners, a 48 px footer on the bare backdrop with its button 30 px from the right edge.
constexpr float kWidth = 360, kPadX = 24, kPadTop = 23, kPadBottom = 24, kRowH = 64, kRowGap = 4, kSectionGap = 24,
                kTileH = 48, kTileLabelH = 48, kTileGap = 12, kFooterH = 48, kCardRadius = 4, kControlRadius = 4,
                kIcon = 16;
constexpr size_t kMaxRows = 6, kTileColumns = 3;

// Opening: the window rises from the taskbar while the content fades in, like the system flyouts.
constexpr float kRiseMs = 200, kFadeMs = 150, kRiseDip = 12;

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t kGlyphSettings = 0xE713, kGlyphRefresh = 0xE72C, kGlyphDevices = 0xE772, kGlyphBatteryUnknown = 0xE996;

template <class T> void Release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

HWND g_wnd;
ID2D1Factory* g_d2d;
IDWriteFactory* g_dw;
ID2D1HwndRenderTarget* g_rt;
ID2D1SolidColorBrush* g_brush;
std::map<std::tuple<std::wstring, float, int, bool>, IDWriteTextFormat*> g_formats;
std::wstring g_iconFont = L"Segoe Fluent Icons";
const wchar_t* kUi = L"Segoe UI Variable Text";

flyout::Model g_model;
flyout::Handler g_handler;
RECT g_anchor{};
float g_scale = 1;  // pixels per DIP
int g_finalX = 0, g_finalY = 0, g_riseDir = 1;  // riseDir: +1 rises up (taskbar below), -1 drops down, 0 none
bool g_motion = true;
ULONGLONG g_openAt = 0, g_closedAt = 0, g_spinAt = 0;
bool g_timer = false;
int g_stage = 99;   // build stage (WB_FLYOUT_STAGE), for checking the backdrop as parts are added
float g_alpha = 1;  // content opacity while it fades in

enum class HitKind { Settings, Refresh, Toggle, Device };
struct Hit {
    D2D1_RECT_F r;
    HitKind kind;
    UINT id = 0;
    std::wstring key;
};
std::vector<Hit> g_hits;
POINT g_mouse{ -1, -1 };
bool g_down = false;
int g_pressed = -1;

// ---- colours: the Windows 11 (Fluent) theme resources for dark and light mode ----------------------

D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return D2D1::ColorF(r, g, b, a); }

struct Palette {
    D2D1_COLOR_F text, textSecondary, textTertiary, layer, card, cardHover, cardPressed, cardStroke, control,
        controlHover, controlPressed, controlStroke, subtleHover, subtlePressed, divider, accent, accentHover,
        accentPressed, onAccent, critical;
};

// The system accent colour as the flyouts use it: "Light 2" in dark mode, "Dark 1" in light mode.
D2D1_COLOR_F Accent(bool light) {
    BYTE palette[32] = {};
    DWORD size = sizeof(palette);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS && size >= 32) {
        const BYTE* c = palette + (light ? 4 * 4 : 1 * 4);
        return Rgba(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
    }
    return light ? Rgba(0, .373f, .722f, 1) : Rgba(.298f, .761f, 1, 1);  // the default blue
}

Palette Colors() {
    D2D1_COLOR_F a = Accent(g_model.light);
    if (g_model.light)
        return { Rgba(0, 0, 0, .896f),    Rgba(0, 0, 0, .606f),   Rgba(0, 0, 0, .446f),   Rgba(1, 1, 1, .50f),
                 Rgba(1, 1, 1, .70f),     Rgba(1, 1, 1, .90f),    Rgba(1, 1, 1, .45f),    Rgba(0, 0, 0, .0578f),
                 Rgba(1, 1, 1, .70f),     Rgba(.976f, .976f, .976f, .5f), Rgba(.976f, .976f, .976f, .3f),
                 Rgba(0, 0, 0, .0578f),   Rgba(0, 0, 0, .0373f),  Rgba(0, 0, 0, .0241f),  Rgba(0, 0, 0, .0803f),
                 a, Rgba(a.r, a.g, a.b, .9f), Rgba(a.r, a.g, a.b, .8f), Rgba(1, 1, 1, 1), Rgba(.769f, .169f, .110f, 1) };
    return { Rgba(1, 1, 1, 1),         Rgba(1, 1, 1, .786f),   Rgba(1, 1, 1, .544f),   Rgba(1, 1, 1, .036f),
             Rgba(1, 1, 1, .0512f),    Rgba(1, 1, 1, .0837f),  Rgba(1, 1, 1, .0326f),  Rgba(0, 0, 0, .10f),
             Rgba(1, 1, 1, .0605f),    Rgba(1, 1, 1, .0837f),  Rgba(1, 1, 1, .0326f),  Rgba(1, 1, 1, .0698f),
             Rgba(1, 1, 1, .0605f),    Rgba(1, 1, 1, .0419f),  Rgba(1, 1, 1, .0837f),
             a, Rgba(a.r, a.g, a.b, .9f), Rgba(a.r, a.g, a.b, .8f), Rgba(0, 0, 0, 1), Rgba(1, .6f, .643f, 1) };
}

ID2D1SolidColorBrush* Brush(D2D1_COLOR_F c) {
    c.a *= g_alpha;
    g_brush->SetColor(c);
    return g_brush;
}

// ---- text -------------------------------------------------------------------------------------

IDWriteTextFormat* Format(const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight, bool wrap = false) {
    auto key = std::make_tuple(family, size, (int)weight, wrap);
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                           L"", &f);
    if (f) {
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* ellipsis = nullptr;
        g_dw->CreateEllipsisTrimmingSign(f, &ellipsis);
        f->SetTrimming(&trim, ellipsis);
        Release(ellipsis);
    }
    g_formats[key] = f;
    return f;
}

// Windows 11 type ramp: Caption 12, Body 14, Body Strong 14 semibold.
IDWriteTextFormat* Caption() { return Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL); }
IDWriteTextFormat* Body() { return Format(kUi, 14, DWRITE_FONT_WEIGHT_NORMAL); }

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (!f || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000, 200, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    Release(l);
    return m.widthIncludingTrailingWhitespace;
}

// One line (or a wrapped block) in `r`, vertically centred; `align` 0 left, 1 centre, 2 right.
void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, int align = 0) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Glyph(wchar_t g, float size, D2D1_RECT_F r, D2D1_COLOR_F c) {
    Text(std::wstring(1, g), Format(g_iconFont, size, DWRITE_FONT_WEIGHT_NORMAL), r, c, 1);
}

wchar_t DeviceGlyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:    return 0xE7F6;
        case DeviceKind::Mouse:      return 0xE962;
        case DeviceKind::Keyboard:   return 0xE765;
        case DeviceKind::Gamepad:    return 0xE7FC;
        case DeviceKind::Phone:      return 0xE8EA;
        case DeviceKind::Microphone: return 0xE720;
        default:                     return bluetooth ? 0xE702 : kGlyphDevices;
    }
}

// The battery icons Windows uses (Battery0..10, BatteryCharging0..10).
wchar_t BatteryGlyph(int percent, bool charging) {
    if (percent < 0) return kGlyphBatteryUnknown;
    int step = std::clamp((percent + 5) / 10, 0, 10);
    if (charging) return step == 10 ? 0xE83E : (wchar_t)(0xE85A + step);
    return step == 10 ? 0xE83F : (wchar_t)(0xE850 + step);
}

// ---- shapes and hit testing -------------------------------------------------------------------

void Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
}

void Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    D2D1_RECT_F in{ r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f };
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - .5f, radius - .5f), Brush(c), 1);
}

bool Inside(const D2D1_RECT_F& r) {
    float x = g_mouse.x / g_scale, y = g_mouse.y / g_scale;
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 0 resting, 1 hovered, 2 pressed.
int State(const D2D1_RECT_F& r) { return !Inside(r) ? 0 : g_down ? 2 : 1; }

// ---- layout -----------------------------------------------------------------------------------

size_t RowCount() { return std::min(g_model.devices.size(), kMaxRows); }
bool MoreLine() { return g_model.devices.size() > kMaxRows; }
size_t TileRows() { return (g_model.toggles.size() + kTileColumns - 1) / kTileColumns; }

float BodyHeight() {
    size_t rows = std::max<size_t>(RowCount(), 1);  // the empty state is one card
    float h = kPadTop + rows * kRowH + (rows - 1) * kRowGap + (MoreLine() ? 24 : 0);
    if (TileRows()) return h + kSectionGap + TileRows() * (kTileH + kTileLabelH);  // the last label area ends it
    return h + kPadBottom;
}

float ContentHeight() { return BodyHeight() + kFooterH; }

float Ease(float t) {  // ease-out cubic
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

float Elapsed(ULONGLONG since) { return since ? (float)(GetTickCount64() - since) : 1e9f; }

bool Animating() {
    if (!g_motion) return false;
    return Elapsed(g_openAt) < std::max(kRiseMs, kFadeMs) + 16 || Elapsed(g_spinAt) < 500;
}

// ---- drawing ----------------------------------------------------------------------------------

void DrawDevice(const Palette& p, const flyout::Device& d, D2D1_RECT_F r) {
    g_hits.push_back({ r, HitKind::Device, 0, d.key });
    int st = State(r);
    Fill(r, kCardRadius, st == 2 ? p.cardPressed : st == 1 ? p.cardHover : p.card);
    Stroke(r, kCardRadius, p.cardStroke);
    float cy = (r.top + r.bottom) / 2;
    D2D1_COLOR_F main = d.dim ? p.textTertiary : p.text;
    Glyph(DeviceGlyph(d.kind, d.bluetooth), kIcon, { r.left + 12, cy - 12, r.left + 36, cy + 12 }, main);

    // Right: battery icon and level, as on the Bluetooth & devices page.
    std::wstring level = !d.levelText.empty() ? d.levelText : d.percent >= 0 ? std::to_wstring(d.percent) + L"%" : L"";
    float lw = level.empty() ? 0 : TextWidth(level, Body());
    float right = r.right - 16;
    if (!level.empty()) Text(level, Body(), { right - lw - 2, cy - 11, right, cy + 11 }, d.low ? p.critical : main, 2);
    float gx = right - lw - 6 - 22;
    Glyph(BatteryGlyph(d.percent, d.charging || d.full), kIcon, { gx, cy - 11, gx + 22, cy + 11 }, d.low ? p.critical : main);

    float tx = r.left + 48, tr = gx - 8;
    Text(d.name, Body(), { tx, cy - 20, tr, cy + 1 }, main);
    Text(d.state, Caption(), { tx, cy + 1, tr, cy + 19 }, p.textSecondary);
}

void DrawEmpty(const Palette& p, D2D1_RECT_F r) {
    Fill(r, kCardRadius, p.card);
    Stroke(r, kCardRadius, p.cardStroke);
    float cy = (r.top + r.bottom) / 2;
    Glyph(kGlyphDevices, kIcon, { r.left + 12, cy - 12, r.left + 36, cy + 12 }, p.textSecondary);
    Text(g_model.text.emptyTitle, Body(), { r.left + 48, cy - 20, r.right - 16, cy + 1 }, p.text);
    Text(g_model.text.emptyBody, Caption(), { r.left + 48, cy + 1, r.right - 16, cy + 19 }, p.textSecondary);
}

// A Quick Settings tile: accent-filled when on, a quiet control when off, its label underneath.
void DrawTile(const Palette& p, const flyout::Toggle& t, D2D1_RECT_F tile) {
    D2D1_RECT_F hit{ tile.left, tile.top, tile.right, tile.bottom + kTileLabelH };
    g_hits.push_back({ hit, HitKind::Toggle, t.id });
    int st = State(hit);
    if (t.on) {
        Fill(tile, kControlRadius, st == 2 ? p.accentPressed : st == 1 ? p.accentHover : p.accent);
    } else {
        Fill(tile, kControlRadius, st == 2 ? p.controlPressed : st == 1 ? p.controlHover : p.control);
        Stroke(tile, kControlRadius, p.controlStroke);
    }
    Glyph(t.glyph, kIcon, tile, t.on ? p.onAccent : p.text);
    // Label centred 16 px under the tile (one line), wrapping to a second line like Quick Settings.
    Text(t.label, Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL, true),
         { tile.left - 6, tile.bottom + 4, tile.right + 6, tile.bottom + 28 }, p.text, 1);
}

void DrawIconButton(const Palette& p, HitKind kind, wchar_t glyph, D2D1_RECT_F r, float spin = 0) {
    g_hits.push_back({ r, kind });
    int st = State(r);
    if (st) Fill(r, kControlRadius, st == 2 ? p.subtlePressed : p.subtleHover);
    if (spin > 0) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(spin, D2D1::Point2F(cx, cy)) *
                           D2D1::Matrix3x2F::Scale(g_scale, g_scale));
    }
    Glyph(glyph, 14, r, p.text);  // the footer settings glyph measures 14 px in Quick Settings
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
}

void Draw() {
    if (!g_rt) return;
    Palette p = Colors();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent: the backdrop shows
    g_hits.clear();
    g_alpha = g_motion && g_stage >= 5 ? Ease(Elapsed(g_openAt) / kFadeMs) : 1;
    const float body = BodyHeight(), x = kPadX, w = kWidth - 2 * kPadX;

    if (g_stage >= 1) {
        // A light layer over the body; the footer stays on the bare backdrop, divided by a hairline.
        g_rt->FillRectangle(D2D1::RectF(0, 0, kWidth, body), Brush(p.layer));
        g_rt->FillRectangle(D2D1::RectF(0, body, kWidth, body + 1), Brush(p.divider));
    }

    float y = kPadTop;
    if (g_stage >= 2) {
        size_t rows = RowCount();
        if (!rows) DrawEmpty(p, { x, y, x + w, y + kRowH });
        for (size_t i = 0; i < rows; i++)
            DrawDevice(p, g_model.devices[i], { x, y + i * (kRowH + kRowGap), x + w, y + i * (kRowH + kRowGap) + kRowH });
        size_t shown = std::max<size_t>(rows, 1);
        y += shown * kRowH + (shown - 1) * kRowGap;
        if (MoreLine()) {
            Text(g_model.text.more, Caption(), { x + 4, y + 2, x + w, y + 22 }, p.textTertiary);
            y += 24;
        }
    }

    if (g_stage >= 3 && TileRows()) {
        y += kSectionGap;
        float tw = (w - kTileGap * (kTileColumns - 1)) / kTileColumns;
        for (size_t i = 0; i < g_model.toggles.size(); i++) {
            size_t row = i / kTileColumns, col = i % kTileColumns;
            float tx = x + col * (tw + kTileGap), ty = y + row * (kTileH + kTileLabelH);
            DrawTile(p, g_model.toggles[i], { tx, ty, tx + tw, ty + kTileH });
        }
    }

    if (g_stage >= 4) {
        float fy = body, cy = fy + kFooterH / 2;
        Text(g_model.text.summary, Caption(), { kPadX, fy, kWidth - 110, fy + kFooterH }, p.textSecondary);
        float spin = g_motion && Elapsed(g_spinAt) < 500 ? Ease(Elapsed(g_spinAt) / 500) * 360 : 0;
        // Icon buttons centred 30 px (settings) and 70 px (refresh) from the right edge.
        DrawIconButton(p, HitKind::Refresh, kGlyphRefresh, { kWidth - 88, cy - 16, kWidth - 52, cy + 16 }, spin);
        DrawIconButton(p, HitKind::Settings, kGlyphSettings, { kWidth - 48, cy - 16, kWidth - 12, cy + 16 });
    }

    g_alpha = 1;
    if (g_rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        Release(g_brush);
        Release(g_rt);
    }
}

// ---- window -----------------------------------------------------------------------------------

void EnsureTarget() {
    if (g_rt) return;
    RECT rc;
    GetClientRect(g_wnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96, 96);
    if (FAILED(g_d2d->CreateHwndRenderTarget(
            props, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt)))
        return;
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    g_rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &g_brush);
}

void StartTimer() {
    if (!g_timer && g_wnd) {
        SetTimer(g_wnd, 1, 10, nullptr);
        g_timer = true;
    }
}

void ApplyWindowPos() {
    float rise = g_motion && g_stage >= 5 ? (1 - Ease(Elapsed(g_openAt) / kRiseMs)) * kRiseDip * g_scale : 0;
    SetWindowPos(g_wnd, HWND_TOPMOST, g_finalX, g_finalY + (int)std::lround(rise) * g_riseDir, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

// DWM keeps drawing Mica's plain grey fallback (32,32,32) instead of the wallpaper-tinted material
// (30,32,36 here, as in Settings) unless the backdrop attributes are applied with frame changes that
// may activate the window: once before it is shown and once more after it is up. Either one alone
// leaves it grey (found by trying each combination).
void ApplyBackdrop() {
    BOOL dark = g_model.light ? FALSE : TRUE;
    DwmSetWindowAttribute(g_wnd, kDwmDarkMode, &dark, sizeof(dark));
    int corners = kCornersRound;
    DwmSetWindowAttribute(g_wnd, kDwmCorners, &corners, sizeof(corners));
    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_wnd, &m);
    int backdrop = g_model.mica ? kBackdropMica : kBackdropAcrylic;
    DwmSetWindowAttribute(g_wnd, kDwmBackdrop, &backdrop, sizeof(backdrop));
    SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
}

// Size and place the window next to the anchor, inside the work area of the anchor's monitor.
void Place() {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&g_anchor, MONITOR_DEFAULTTONEAREST), &mi);
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    static auto getDpi = (GetDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    // Move onto the monitor first so the DPI read below is that monitor's.
    SetWindowPos(g_wnd, nullptr, mi.rcWork.left, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (getDpi) dpi = getDpi(g_wnd);
    g_scale = dpi / 96.f;
    int w = (int)std::lround(kWidth * g_scale), h = (int)std::lround(ContentHeight() * g_scale);
    int gap = (int)std::lround(12 * g_scale);
    const RECT& wa = mi.rcWork;
    const RECT& a = g_anchor;
    int x, y;
    if (a.right <= wa.left + 4 || a.left >= wa.right - 4) {  // vertical taskbar: beside it
        x = a.right <= wa.left + 4 ? wa.left + gap : wa.right - gap - w;
        y = std::clamp((int)((a.top + a.bottom) / 2 - h / 2), (int)wa.top + gap, (int)wa.bottom - gap - h);
        g_riseDir = 0;
    } else {
        x = std::clamp((int)((a.left + a.right) / 2 - w / 2), (int)wa.left + gap, (int)wa.right - gap - w);
        if (a.bottom <= wa.top + 4) { y = wa.top + gap; g_riseDir = -1; }            // taskbar at the top
        else if (a.top >= wa.bottom - 4) { y = wa.bottom - gap - h; g_riseDir = 1; }  // taskbar at the bottom
        else { y = std::max((int)wa.top + gap, (int)a.top - gap - h); g_riseDir = 1; }  // tray icon / elsewhere
    }
    g_finalX = x;
    g_finalY = y;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    if (g_rt) g_rt->Resize(D2D1::SizeU(w, h));
}

void Activate(const Hit& hit) {
    flyout::Handler handler = g_handler;
    switch (hit.kind) {
        case HitKind::Settings: flyout::Hide(); if (handler) handler(flyout::Command::Settings, 0, L""); break;
        case HitKind::Device: flyout::Hide(); if (handler) handler(flyout::Command::History, 0, hit.key); break;
        case HitKind::Refresh:
            g_spinAt = GetTickCount64();
            StartTimer();
            if (handler) handler(flyout::Command::Refresh, 0, L"");
            break;
        case HitKind::Toggle:
            if (handler) handler(flyout::Command::Toggle, hit.id, L"");
            break;
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

int HitAt(POINT pt) {
    float x = pt.x / g_scale, y = pt.y / g_scale;
    for (size_t i = 0; i < g_hits.size(); i++) {
        const auto& r = g_hits[i].r;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return (int)i;
    }
    return -1;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EnsureTarget();
            Draw();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_TIMER:
            if (Elapsed(g_openAt) < kRiseMs + 20) ApplyWindowPos();
            InvalidateRect(h, nullptr, FALSE);
            if (!Animating()) {
                KillTimer(h, 1);
                g_timer = false;
                ApplyWindowPos();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(h)) {
                flyout::Hide();
                g_closedAt = GetTickCount64();
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) flyout::Hide();
            return 0;
        case WM_MOUSEMOVE: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            int before = HitAt(g_mouse), after = HitAt(pt);
            g_mouse = pt;
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            if (before != after) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_mouse = { -1, -1 };
            g_down = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            g_down = true;
            g_pressed = HitAt({ (short)LOWORD(lp), (short)HIWORD(lp) });
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            g_down = false;
            int hit = HitAt({ (short)LOWORD(lp), (short)HIWORD(lp) });
            int pressed = g_pressed;
            g_pressed = -1;
            if (hit >= 0 && hit == pressed) {
                Hit copy = g_hits[hit];
                Activate(copy);
            } else {
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED: return 0;  // placed by Place() for the anchor's monitor
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Init(HINSTANCE inst) {
    if (g_wnd) return true;
    if (const char* s = getenv("WB_FLYOUT_STAGE")) g_stage = atoi(s);
    if (!g_d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (!g_dw && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown**>(&g_dw))))
        return false;
    // Segoe Fluent Icons ships with Windows 11; Segoe MDL2 Assets has the same code points.
    IDWriteFontCollection* fonts = nullptr;
    if (SUCCEEDED(g_dw->GetSystemFontCollection(&fonts, FALSE))) {
        UINT32 index;
        BOOL exists = FALSE;
        fonts->FindFamilyName(L"Segoe Fluent Icons", &index, &exists);
        if (!exists) g_iconFont = L"Segoe MDL2 Assets";
        Release(fonts);
    }
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"WirelessBatteryFlyout";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"BatteryHub", WS_POPUP, 0, 0, 10, 10,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return false;
    int backdrop = kBackdropMica;
    if (FAILED(DwmSetWindowAttribute(g_wnd, kDwmBackdrop, &backdrop, sizeof(backdrop)))) {  // before Windows 11 22H2
        DestroyWindow(g_wnd);
        g_wnd = nullptr;
        return false;
    }
    return true;
}

}  // namespace

namespace flyout {

bool Show(HINSTANCE inst, const RECT& anchor, const Model& model, Handler handler) {
    if (!Init(inst)) return false;
    g_model = model;
    g_handler = std::move(handler);
    g_anchor = anchor;
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    g_motion = anim != FALSE;
    g_mouse = { -1, -1 };
    g_down = false;
    g_pressed = -1;
    g_spinAt = 0;
    ApplyBackdrop();
    Place();
    g_openAt = GetTickCount64();
    ApplyWindowPos();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
    UpdateWindow(g_wnd);
    ApplyBackdrop();
    if (g_motion && g_stage >= 5) StartTimer();
    return true;
}

void Update(const Model& model) {
    if (!Visible()) return;
    bool resize = model.devices.size() != g_model.devices.size() || model.toggles.size() != g_model.toggles.size();
    bool theme = model.light != g_model.light || model.mica != g_model.mica;
    g_model = model;
    if (theme) ApplyBackdrop();
    if (resize) Place();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void Hide() {
    if (!g_wnd) return;
    if (g_timer) {
        KillTimer(g_wnd, 1);
        g_timer = false;
    }
    ShowWindow(g_wnd, SW_HIDE);
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }

bool JustClosed() { return g_closedAt && GetTickCount64() - g_closedAt < 250; }

}  // namespace flyout
