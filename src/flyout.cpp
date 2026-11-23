#include "flyout.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace {

// DWM attributes, defined here because older SDK / MinGW headers lack them.
constexpr DWORD kDwmDarkMode = 20;    // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmCorners = 33;     // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBackdrop = 38;    // DWMWA_SYSTEMBACKDROP_TYPE
constexpr int kBackdropMica = 2, kBackdropAcrylic = 3, kCornersRound = 2;

// Layout, in DIPs.
constexpr float kWidth = 368, kPad = 16, kHeaderH = 54, kHeroH = 132, kRowH = 54, kTileH = 50, kGap = 8,
                kActionsH = 72, kRadius = 20;
constexpr size_t kMaxRows = 6;

// Animation, in ms: the window rises a little while its sections fade in one after another.
constexpr float kRiseMs = 220, kSectionMs = 240, kStaggerMs = 35, kSwapMs = 160, kSpinMs = 600;
constexpr float kRiseDip = 10;

// Segoe Fluent Icons / Segoe MDL2 Assets code points.
constexpr wchar_t kGlyphSettings = 0xE713, kGlyphRefresh = 0xE72C, kGlyphHistory = 0xE81C, kGlyphPower = 0xE7E8,
                  kGlyphBolt = 0xE945, kGlyphCheck = 0xE73E, kGlyphDevices = 0xE772;

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

flyout::Model g_model;
flyout::Handler g_handler;
std::wstring g_heroKey;  // device the user picked to show large (kept while the app runs)
RECT g_anchor{};
float g_heightDip = 400;
float g_scale = 1;       // pixels per DIP
int g_finalX = 0, g_finalY = 0, g_riseDir = 1;  // riseDir: +1 rises up (taskbar below), -1 drops down
bool g_motion = true;
ULONGLONG g_openAt = 0, g_swapAt = 0, g_spinAt = 0, g_closedAt = 0;
bool g_timer = false;

enum class HitKind { Settings, Refresh, History, Exit, Toggle, Device };
struct Hit {
    D2D1_RECT_F r;
    HitKind kind;
    UINT id = 0;
    std::wstring key;
};
std::vector<Hit> g_hits;
POINT g_mouse{ -1, -1 };
int g_pressed = -1;

float g_alpha = 1;  // opacity of the section being drawn
float g_dy = 0;     // its vertical offset while it fades in

// ---- colours ----------------------------------------------------------------------------------

struct Palette {
    D2D1_COLOR_F text, sub, faint, card, cardHover, cardPress, border, accent, tint, icon, iconOn, iconOnText, low;
};

D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return D2D1::ColorF(r, g, b, a); }

Palette Colors() {
    if (g_model.light)
        return { Rgba(.10f, .11f, .12f, 1), Rgba(.10f, .11f, .12f, .62f), Rgba(.10f, .11f, .12f, .42f),
                 Rgba(1, 1, 1, .55f), Rgba(1, 1, 1, .75f), Rgba(1, 1, 1, .90f), Rgba(1, 1, 1, .85f),
                 Rgba(.07f, .52f, .35f, 1), Rgba(.30f, .78f, .55f, .20f), Rgba(1, 1, 1, .85f),
                 Rgba(.10f, .11f, .12f, 1), Rgba(1, 1, 1, 1), Rgba(.77f, .12f, .16f, 1) };
    return { Rgba(1, 1, 1, 1), Rgba(1, 1, 1, .64f), Rgba(1, 1, 1, .40f),
             Rgba(1, 1, 1, .065f), Rgba(1, 1, 1, .11f), Rgba(1, 1, 1, .15f), Rgba(1, 1, 1, .09f),
             Rgba(.35f, .87f, .60f, 1), Rgba(.30f, .85f, .58f, .16f), Rgba(1, 1, 1, .10f),
             Rgba(1, 1, 1, .94f), Rgba(.09f, .10f, .11f, 1), Rgba(1, .45f, .45f, 1) };
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

const wchar_t* kUi = L"Segoe UI Variable Text";
const wchar_t* kDisplay = L"Segoe UI Variable Display";

float TextWidth(const std::wstring& s, IDWriteTextFormat* f) {
    IDWriteTextLayout* l = nullptr;
    if (!f || FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 2000, 200, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    Release(l);
    return m.widthIncludingTrailingWhitespace;
}

// Draws one line of text in `r`, vertically centred; `align` 0 left, 1 centre, 2 right.
void Text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c, int align = 0) {
    if (!f || s.empty()) return;
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    r.top += g_dy;
    r.bottom += g_dy;
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

// ---- shapes -----------------------------------------------------------------------------------

D2D1_RECT_F Shift(D2D1_RECT_F r) { return { r.left, r.top + g_dy, r.right, r.bottom + g_dy }; }

void Card(D2D1_RECT_F r, float radius, D2D1_COLOR_F fill, D2D1_COLOR_F border) {
    r = Shift(r);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(fill));
    D2D1_RECT_F in{ r.left + .5f, r.top + .5f, r.right - .5f, r.bottom - .5f };
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius - .5f, radius - .5f), Brush(border), 1);
}

void Circle(float cx, float cy, float rad, D2D1_COLOR_F fill) {
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + g_dy), rad, rad), Brush(fill));
}

bool Inside(const D2D1_RECT_F& r) {
    float x = g_mouse.x / g_scale, y = g_mouse.y / g_scale;
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Fill for something clickable: resting, hovered or pressed.
D2D1_COLOR_F Interactive(const Palette& p, const D2D1_RECT_F& r, D2D1_COLOR_F rest) {
    if (!Inside(r)) return rest;
    bool pressed = g_pressed >= 0 && (GetKeyState(VK_LBUTTON) & 0x8000);
    return pressed ? p.cardPress : p.cardHover;
}

// ---- model helpers ----------------------------------------------------------------------------

int HeroIndex() {
    for (size_t i = 0; i < g_model.devices.size(); i++)
        if (g_model.devices[i].key == g_heroKey) return (int)i;
    return g_model.devices.empty() ? -1 : 0;
}

size_t RowCount() {
    size_t n = g_model.devices.size();
    return n > 1 ? std::min(n - 1, kMaxRows) : 0;
}

float ContentHeight() {
    size_t rows = RowCount();
    bool more = g_model.devices.size() > kMaxRows + 1;
    size_t tileRows = (g_model.toggles.size() + 1) / 2;
    float h = kPad + kHeaderH + kHeroH;
    if (rows) h += kGap + rows * kRowH + (more ? 22 : 0);
    if (tileRows) h += kGap + 4 + tileRows * kTileH + (tileRows - 1) * kGap;
    h += kGap + kActionsH + kPad - 6;
    return h;
}

float Ease(float t) {  // ease-out cubic
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}

float Elapsed(ULONGLONG since) { return since ? (float)(GetTickCount64() - since) : 1e9f; }

// Sets g_alpha / g_dy for section `i` of the opening animation.
void Section(int i) {
    if (!g_motion) { g_alpha = 1; g_dy = 0; return; }
    float t = Ease((Elapsed(g_openAt) - i * kStaggerMs) / kSectionMs);
    g_alpha = t;
    g_dy = (1 - t) * 8 * g_riseDir;
}

bool Animating() {
    if (!g_motion) return false;
    float open = Elapsed(g_openAt), lastSection = kStaggerMs * 4 + kSectionMs;
    return open < std::max(kRiseMs, lastSection) || Elapsed(g_swapAt) < kSwapMs || Elapsed(g_spinAt) < kSpinMs;
}

// ---- drawing ----------------------------------------------------------------------------------

void DrawHero(const Palette& p, D2D1_RECT_F r) {
    int hi = HeroIndex();
    if (hi < 0) {  // nothing found yet: an invitation instead of an empty card
        Card(r, kRadius, p.card, p.border);
        Glyph(kGlyphDevices, 44, { r.right - 110, r.top, r.right - 18, r.bottom }, p.faint);
        Text(g_model.text.emptyTitle, Format(kUi, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             { r.left + 18, r.top + 34, r.right - 120, r.top + 60 }, p.text);
        Text(g_model.text.emptyBody, Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL),
             { r.left + 18, r.top + 62, r.right - 120, r.top + 98 }, p.sub);
        return;
    }
    const auto& d = g_model.devices[hi];
    bool tinted = d.charging || d.full;
    Card(r, kRadius, tinted ? p.tint : p.card, p.border);
    float swap = g_motion ? Ease(Elapsed(g_swapAt) / kSwapMs) : 1;
    float keep = g_alpha;
    g_alpha *= swap;
    D2D1_COLOR_F numColor = d.dim ? p.sub : d.low ? p.low : p.text;
    float x = r.left + 20;
    Text(d.name, Format(kUi, 13, DWRITE_FONT_WEIGHT_NORMAL), { x, r.top + 16, r.right - 128, r.top + 36 }, p.sub);
    if (d.percent >= 0 && d.levelText.empty()) {
        std::wstring num = std::to_wstring(d.percent);
        IDWriteTextFormat* big = Format(kDisplay, 56, DWRITE_FONT_WEIGHT_SEMI_LIGHT);
        float w = TextWidth(num, big);
        Text(num, big, { x - 2, r.top + 34, x + w + 4, r.top + 98 }, numColor);
        Text(L"%", Format(kDisplay, 26, DWRITE_FONT_WEIGHT_SEMI_LIGHT), { x + w + 1, r.top + 54, x + w + 40, r.top + 92 },
             p.faint);
    } else {
        std::wstring t = !d.levelText.empty() ? d.levelText : L"--";
        Text(t, Format(kDisplay, 40, DWRITE_FONT_WEIGHT_SEMI_LIGHT), { x - 1, r.top + 38, r.right - 128, r.top + 94 },
             numColor);
    }
    // State line: a bolt or check mark in green while on power, a small dot otherwise.
    float sy = r.top + 100;
    if (d.charging || d.full) {
        Glyph(d.full ? kGlyphCheck : kGlyphBolt, 13, { x - 2, sy, x + 14, sy + 18 }, p.accent);
        Text(d.state, Format(kUi, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD), { x + 17, sy, r.right - 128, sy + 18 }, p.accent);
    } else {
        Circle(x + 4, sy + 9, 3, p.faint);
        Text(d.state, Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL), { x + 14, sy, r.right - 128, sy + 18 }, p.sub);
    }
    // The device itself, large, with a soft contact shadow under it.
    float gx = r.right - 124, gy = r.top + 6;
    g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(gx + 56, gy + 106 + g_dy), 28, 3.5f),
                      Brush(Rgba(0, 0, 0, g_model.light ? .08f : .18f)));
    Glyph(DeviceGlyph(d.kind, d.bluetooth), 78, { gx, gy, gx + 112, gy + 112 }, d.dim ? p.sub : p.text);
    g_alpha = keep;
}

void DrawRow(const Palette& p, const flyout::Device& d, D2D1_RECT_F r) {
    g_hits.push_back({ r, HitKind::Device, 0, d.key });
    D2D1_COLOR_F fill = Interactive(p, r, Rgba(0, 0, 0, 0));
    if (fill.a > 0) {
        D2D1_RECT_F s = Shift(r);
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(s, 16, 16), Brush(fill));
    }
    float cy = (r.top + r.bottom) / 2;
    Circle(r.left + 28, cy, 19, p.icon);
    Glyph(DeviceGlyph(d.kind, d.bluetooth), 17, { r.left + 9, cy - 19, r.left + 47, cy + 19 }, d.dim ? p.sub : p.text);
    float tx = r.left + 58, right = r.right - 70;
    Text(d.name, Format(kUi, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD), { tx, cy - 19, right, cy + 1 }, d.dim ? p.sub : p.text);
    Text(d.state, Format(kUi, 11, d.charging || d.full ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL),
         { tx, cy + 1, right, cy + 19 }, d.charging || d.full ? p.accent : p.sub);
    D2D1_COLOR_F numColor = d.dim ? p.sub : d.low ? p.low : p.text;
    if (d.percent >= 0 && d.levelText.empty()) {
        IDWriteTextFormat* pct = Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL);
        float pw = TextWidth(L"%", pct);
        Text(L"%", pct, { r.right - 12 - pw, cy - 6, r.right - 12, cy + 10 }, p.faint);
        Text(std::to_wstring(d.percent), Format(kDisplay, 19, DWRITE_FONT_WEIGHT_NORMAL),
             { r.right - 90, cy - 12, r.right - 13 - pw, cy + 12 }, numColor, 2);
    } else {
        Text(!d.levelText.empty() ? d.levelText : L"--", Format(kUi, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD),
             { r.right - 90, cy - 12, r.right - 12, cy + 12 }, numColor, 2);
    }
}

void DrawTile(const Palette& p, const flyout::Toggle& t, D2D1_RECT_F r) {
    g_hits.push_back({ r, HitKind::Toggle, t.id });
    Card(r, 18, Interactive(p, r, p.card), p.border);
    float cy = (r.top + r.bottom) / 2;
    Circle(r.left + 25, cy, 15, t.on ? p.iconOn : p.icon);
    Glyph(t.glyph, 14, { r.left + 10, cy - 15, r.left + 40, cy + 15 }, t.on ? p.iconOnText : p.text);
    const std::wstring& state = t.on ? g_model.text.on : g_model.text.off;
    IDWriteTextFormat* sf = Format(kUi, 11, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float sw = TextWidth(state, sf);
    Text(state, sf, { r.right - 12 - sw, cy - 9, r.right - 12, cy + 9 }, t.on ? p.accent : p.faint, 2);
    Text(t.label, Format(kUi, 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, true), { r.left + 48, cy - 17, r.right - 16 - sw, cy + 17 },
         p.text);  // long labels wrap onto a second line
}

void DrawAction(const Palette& p, HitKind kind, wchar_t glyph, const std::wstring& label, float cx, float top) {
    D2D1_RECT_F r{ cx - 40, top + 6, cx + 40, top + kActionsH - 6 };
    g_hits.push_back({ r, kind });
    if (Inside(r)) {
        D2D1_RECT_F s = Shift(r);
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(s, 16, 16), Brush(Interactive(p, r, p.card)));
    }
    Glyph(glyph, 18, { cx - 20, top + 14, cx + 20, top + 38 }, p.text);
    Text(label, Format(kUi, 11, DWRITE_FONT_WEIGHT_NORMAL), { cx - 40, top + 40, cx + 40, top + 58 }, p.sub, 1);
}

void Draw() {
    if (!g_rt) return;
    Palette p = Colors();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));
    g_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    g_hits.clear();
    const float x = kPad, w = kWidth - 2 * kPad;
    float y = kPad;

    // Header: greeting, one-line summary, and the way to every other setting.
    Section(0);
    Text(g_model.text.greeting, Format(kUi, 12, DWRITE_FONT_WEIGHT_NORMAL), { x + 4, y, x + w - 56, y + 18 }, p.sub);
    Text(g_model.text.summary, Format(kUi, 16, DWRITE_FONT_WEIGHT_SEMI_BOLD), { x + 4, y + 17, x + w - 56, y + 41 },
         p.text);
    D2D1_RECT_F settings{ x + w - 40, y, x + w, y + 40 };
    g_hits.push_back({ settings, HitKind::Settings });
    Card(settings, 20, Interactive(p, settings, p.card), p.border);
    Glyph(kGlyphSettings, 16, settings, p.text);
    y += kHeaderH;

    Section(1);
    DrawHero(p, { x, y, x + w, y + kHeroH });
    y += kHeroH;

    size_t rows = RowCount();
    if (rows) {
        Section(2);
        y += kGap;
        int hi = HeroIndex();
        size_t drawn = 0;
        for (size_t i = 0; i < g_model.devices.size() && drawn < rows; i++) {
            if ((int)i == hi) continue;
            DrawRow(p, g_model.devices[i], { x - 4, y, x + w + 4, y + kRowH });
            y += kRowH;
            drawn++;
        }
        if (g_model.devices.size() > kMaxRows + 1) {
            Text(g_model.text.more, Format(kUi, 11, DWRITE_FONT_WEIGHT_NORMAL), { x + 4, y, x + w, y + 20 }, p.faint);
            y += 22;
        }
    }

    if (!g_model.toggles.empty()) {
        Section(3);
        y += kGap + 4;
        float tw = (w - kGap) / 2;
        for (size_t i = 0; i < g_model.toggles.size(); i++) {
            float tx = x + (i % 2) * (tw + kGap);
            DrawTile(p, g_model.toggles[i], { tx, y, tx + tw, y + kTileH });
            if (i % 2 == 1 || i + 1 == g_model.toggles.size()) y += kTileH + (i + 1 < g_model.toggles.size() ? kGap : 0);
        }
    }

    // Actions: history of the device shown large, a round refresh button in the middle, exit.
    Section(4);
    y += kGap;
    DrawAction(p, HitKind::History, kGlyphHistory, g_model.text.history, x + w * 0.18f, y);
    DrawAction(p, HitKind::Exit, kGlyphPower, g_model.text.exit, x + w * 0.82f, y);
    float cx = x + w / 2, cy = y + kActionsH / 2;
    D2D1_RECT_F refresh{ cx - 28, cy - 28, cx + 28, cy + 28 };
    g_hits.push_back({ refresh, HitKind::Refresh });
    Card(refresh, 28, Interactive(p, refresh, g_model.light ? Rgba(1, 1, 1, .85f) : Rgba(1, 1, 1, .12f)), p.border);
    float spin = g_motion && Elapsed(g_spinAt) < kSpinMs ? Ease(Elapsed(g_spinAt) / kSpinMs) * 360 : 0;
    if (spin > 0) {
        D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(g_scale, g_scale);
        g_rt->SetTransform(D2D1::Matrix3x2F::Rotation(spin, D2D1::Point2F(cx, cy + g_dy)) * base);
    }
    Glyph(kGlyphRefresh, 20, refresh, p.text);
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_scale, g_scale));

    g_alpha = 1;
    g_dy = 0;
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
    float rise = g_motion ? (1 - Ease(Elapsed(g_openAt) / kRiseMs)) * kRiseDip * g_scale : 0;
    SetWindowPos(g_wnd, HWND_TOPMOST, g_finalX, g_finalY + (int)std::lround(rise) * g_riseDir, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
}

void ApplyTheme() {
    BOOL dark = g_model.light ? FALSE : TRUE;
    DwmSetWindowAttribute(g_wnd, kDwmDarkMode, &dark, sizeof(dark));
    int backdrop = g_model.mica ? kBackdropMica : kBackdropAcrylic;
    DwmSetWindowAttribute(g_wnd, kDwmBackdrop, &backdrop, sizeof(backdrop));
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
    g_heightDip = ContentHeight();
    int w = (int)std::lround(kWidth * g_scale), h = (int)std::lround(g_heightDip * g_scale);
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
        if (a.bottom <= wa.top + 4) { y = wa.top + gap; g_riseDir = -1; }           // taskbar at the top
        else if (a.top >= wa.bottom - 4) { y = wa.bottom - gap - h; g_riseDir = 1; } // taskbar at the bottom
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
        case HitKind::Exit: flyout::Hide(); if (handler) handler(flyout::Command::Exit, 0, L""); break;
        case HitKind::History: {
            int hi = HeroIndex();
            std::wstring key = hi >= 0 ? g_model.devices[hi].key : L"";
            flyout::Hide();
            if (handler && !key.empty()) handler(flyout::Command::History, 0, key);
            break;
        }
        case HitKind::Refresh:
            g_spinAt = GetTickCount64();
            StartTimer();
            if (handler) handler(flyout::Command::Refresh, 0, L"");
            break;
        case HitKind::Toggle:
            if (handler) handler(flyout::Command::Toggle, hit.id, L"");
            break;
        case HitKind::Device:
            if (hit.key != g_heroKey) {
                g_heroKey = hit.key;
                g_swapAt = GetTickCount64();
                StartTimer();
            }
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
        case WM_TIMER: {
            if (Elapsed(g_openAt) < kRiseMs + 20) ApplyWindowPos();
            InvalidateRect(h, nullptr, FALSE);
            if (!Animating()) {
                KillTimer(h, 1);
                g_timer = false;
                ApplyWindowPos();
            }
            return 0;
        }
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
            SetCursor(LoadCursorW(nullptr, after >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) return TRUE;  // set in WM_MOUSEMOVE
            break;
        case WM_MOUSELEAVE:
            g_mouse = { -1, -1 };
            g_pressed = -1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            g_pressed = HitAt({ (short)LOWORD(lp), (short)HIWORD(lp) });
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
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
    int corners = kCornersRound;
    DwmSetWindowAttribute(g_wnd, kDwmCorners, &corners, sizeof(corners));
    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_wnd, &m);
    int backdrop = kBackdropAcrylic;
    if (FAILED(DwmSetWindowAttribute(g_wnd, kDwmBackdrop, &backdrop, sizeof(backdrop)))) {
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
    g_pressed = -1;
    g_spinAt = g_swapAt = 0;
    ApplyTheme();
    Place();
    g_openAt = GetTickCount64();
    if (g_motion) ApplyWindowPos();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
    UpdateWindow(g_wnd);
    if (g_motion) StartTimer();
    return true;
}

void Update(const Model& model) {
    if (!Visible()) return;
    bool resize = model.devices.size() != g_model.devices.size() || model.toggles.size() != g_model.toggles.size();
    bool theme = model.light != g_model.light || model.mica != g_model.mica;
    g_model = model;
    if (theme) ApplyTheme();
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
