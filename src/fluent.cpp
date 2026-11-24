#include "fluent.h"

#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace fluent {
namespace {

constexpr DWORD kDwmDarkMode = 20;  // DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD kDwmCorners = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmBackdrop = 38;  // DWMWA_SYSTEMBACKDROP_TYPE: 1 none, 2 Mica, 3 Acrylic
const wchar_t* kUi = L"Segoe UI Variable Text";
const wchar_t* kDisplay = L"Segoe UI Variable Display";

template <class T> void Release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

ID2D1Factory* Factory() {
    static ID2D1Factory* f = nullptr;
    if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
    return f;
}

std::wstring IconFont() {
    static std::wstring font;
    if (font.empty()) {
        font = L"Segoe Fluent Icons";  // Windows 11; Segoe MDL2 Assets has the same code points
        IDWriteFontCollection* fonts = nullptr;
        if (DWrite() && SUCCEEDED(DWrite()->GetSystemFontCollection(&fonts, FALSE))) {
            UINT32 index;
            BOOL exists = FALSE;
            fonts->FindFamilyName(font.c_str(), &index, &exists);
            if (!exists) font = L"Segoe MDL2 Assets";
            Release(fonts);
        }
    }
    return font;
}

IDWriteTextFormat* Format(const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight, bool wrap) {
    static std::map<std::tuple<std::wstring, float, int, bool>, IDWriteTextFormat*> cache;
    auto key = std::make_tuple(family, size, (int)weight, wrap);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    if (DWrite())
        DWrite()->CreateTextFormat(family.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   size, L"", &f);
    if (f) {
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* ellipsis = nullptr;
        DWrite()->CreateEllipsisTrimmingSign(f, &ellipsis);
        f->SetTrimming(&trim, ellipsis);
        Release(ellipsis);
    }
    cache[key] = f;
    return f;
}

// Large sizes use the Display optical size of Segoe UI Variable, as Windows does for titles.
const wchar_t* FamilyFor(float size) { return size >= 20 ? kDisplay : kUi; }

D2D1_COLOR_F Rgba(float r, float g, float b, float a) { return D2D1::ColorF(r, g, b, a); }

// The accent as Windows uses it: "Light 2" in dark mode, "Dark 1" in light mode.
D2D1_COLOR_F AccentColor(bool light) {
    BYTE palette[32] = {};
    DWORD size = sizeof(palette);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, palette, &size) == ERROR_SUCCESS && size >= 32) {
        const BYTE* c = palette + (light ? 4 * 4 : 1 * 4);
        return Rgba(c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, 1);
    }
    return light ? Rgba(0, .373f, .722f, 1) : Rgba(.298f, .761f, 1, 1);
}

}  // namespace

IDWriteFactory* DWrite() {
    static IDWriteFactory* f = nullptr;
    if (!f) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&f));
    return f;
}

Palette Colors(bool light) {
    D2D1_COLOR_F a = AccentColor(light);
    Palette p;
    if (light) {
        p.text = Rgba(0, 0, 0, .896f); p.textSecondary = Rgba(0, 0, 0, .606f); p.textTertiary = Rgba(0, 0, 0, .446f);
        p.textDisabled = Rgba(0, 0, 0, .361f);
        p.layer = Rgba(1, 1, 1, .30f);
        p.card = Rgba(1, 1, 1, .70f); p.cardSecondary = Rgba(.965f, .965f, .965f, .50f);
        p.cardHover = Rgba(.976f, .976f, .976f, .85f); p.cardPressed = Rgba(.976f, .976f, .976f, .45f);
        p.cardStroke = Rgba(0, 0, 0, .0578f);
        p.control = Rgba(1, 1, 1, .70f); p.controlHover = Rgba(.976f, .976f, .976f, .50f);
        p.controlPressed = Rgba(.976f, .976f, .976f, .30f); p.controlStroke = Rgba(0, 0, 0, .0578f);
        p.controlStrongStroke = Rgba(0, 0, 0, .446f);
        p.subtleHover = Rgba(0, 0, 0, .0373f); p.subtlePressed = Rgba(0, 0, 0, .0241f); p.divider = Rgba(0, 0, 0, .0803f);
        p.onAccent = Rgba(1, 1, 1, 1);
        p.popup = Rgba(.988f, .988f, .988f, .98f); p.popupStroke = Rgba(0, 0, 0, .0578f);
        p.critical = Rgba(.769f, .169f, .110f, 1); p.success = Rgba(.059f, .482f, .059f, 1);
    } else {
        p.text = Rgba(1, 1, 1, 1); p.textSecondary = Rgba(1, 1, 1, .786f); p.textTertiary = Rgba(1, 1, 1, .544f);
        p.textDisabled = Rgba(1, 1, 1, .363f);
        p.layer = Rgba(.23f, .23f, .23f, .18f);
        p.card = Rgba(1, 1, 1, .0512f); p.cardSecondary = Rgba(1, 1, 1, .0326f);
        p.cardHover = Rgba(1, 1, 1, .0837f); p.cardPressed = Rgba(1, 1, 1, .0326f); p.cardStroke = Rgba(0, 0, 0, .10f);
        p.control = Rgba(1, 1, 1, .0605f); p.controlHover = Rgba(1, 1, 1, .0837f); p.controlPressed = Rgba(1, 1, 1, .0326f);
        p.controlStroke = Rgba(1, 1, 1, .0698f); p.controlStrongStroke = Rgba(1, 1, 1, .544f);
        p.subtleHover = Rgba(1, 1, 1, .0605f); p.subtlePressed = Rgba(1, 1, 1, .0419f); p.divider = Rgba(1, 1, 1, .0837f);
        p.onAccent = Rgba(0, 0, 0, 1);
        p.popup = Rgba(.173f, .173f, .173f, .98f); p.popupStroke = Rgba(0, 0, 0, .20f);
        p.critical = Rgba(1, .6f, .643f, 1); p.success = Rgba(.424f, .796f, .373f, 1);
    }
    p.accent = a;
    p.accentHover = Rgba(a.r, a.g, a.b, .9f);
    p.accentPressed = Rgba(a.r, a.g, a.b, .8f);
    return p;
}

wchar_t DeviceGlyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:    return 0xE7F6;
        case DeviceKind::Mouse:      return 0xE962;
        case DeviceKind::Keyboard:   return 0xE765;
        case DeviceKind::Gamepad:    return 0xE7FC;
        case DeviceKind::Phone:      return 0xE8EA;
        case DeviceKind::Microphone: return 0xE720;
        default:                     return bluetooth ? 0xE702 : glyph::Devices;
    }
}

wchar_t BatteryGlyph(int percent, bool charging) {
    if (percent < 0) return glyph::BatteryUnknown;
    int step = std::clamp((percent + 5) / 10, 0, 10);
    if (charging) return step == 10 ? 0xE83E : (wchar_t)(0xE85A + step);
    return step == 10 ? 0xE83F : (wchar_t)(0xE850 + step);
}

// ---- Painter ----------------------------------------------------------------------------------

bool Painter::Begin(HWND wnd) {
    if (wnd_ != wnd) {
        Release(brush_);
        Release(window_);
        wnd_ = wnd;
    }
    if (!window_) {
        RECT rc;
        GetClientRect(wnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (!Factory() || FAILED(Factory()->CreateHwndRenderTarget(
                              props, D2D1::HwndRenderTargetProperties(wnd, D2D1::SizeU(rc.right, rc.bottom)), &window_)))
            return false;
        window_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        window_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &brush_);
        generation_++;
    }
    rt_ = window_;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent: the backdrop shows
    return true;
}

void Painter::End() {
    if (!window_) return;
    if (window_->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
        Release(brush_);
        Release(window_);
        rt_ = nullptr;
    }
}

void Painter::Resize(UINT width, UINT height) {
    if (!window_) return;
    window_->Resize(D2D1::SizeU(width, height));
    generation_++;
}

bool Painter::BeginLayer() {
    if (!window_ || layer_) return false;
    // Same size and format as the window; it shares the window target's brushes.
    if (FAILED(window_->CreateCompatibleRenderTarget(nullptr, nullptr, nullptr, D2D1_COMPATIBLE_RENDER_TARGET_OPTIONS_NONE, &layer_)))
        return false;
    rt_ = layer_;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return true;
}

ID2D1Bitmap* Painter::EndLayer() {
    ID2D1Bitmap* bitmap = nullptr;
    if (!layer_) return nullptr;
    if (SUCCEEDED(layer_->EndDraw())) layer_->GetBitmap(&bitmap);
    Release(layer_);
    rt_ = window_;
    return bitmap;
}

void Painter::DrawLayer(ID2D1Bitmap* layer) {
    if (!layer) return;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());  // at the origin, pixel for pixel
    rt_->DrawBitmap(layer, nullptr, alpha, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
}

ID2D1SolidColorBrush* Painter::Brush(D2D1_COLOR_F c) {
    c.a *= alpha;
    brush_->SetColor(c);
    return brush_;
}

void Painter::Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    if (radius > 0) rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    else rt_->FillRectangle(r, Brush(c));
}

void Painter::Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width) {
    float h = width / 2;
    D2D1_RECT_F in{ r.left + h, r.top + h, r.right - h, r.bottom - h };
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.f, radius - h), std::max(0.f, radius - h)), Brush(c), width);
}

void Painter::Rect(D2D1_RECT_F r, D2D1_COLOR_F c) { rt_->FillRectangle(r, Brush(c)); }

void Painter::Circle(float cx, float cy, float radius, D2D1_COLOR_F c) {
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), Brush(c));
}

void Painter::Text(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight, D2D1_RECT_F r, D2D1_COLOR_F c,
                   int align, bool wrap) {
    IDWriteTextFormat* f = Format(FamilyFor(size), size, weight, wrap);
    if (!f || s.empty()) return;
    f->SetTextAlignment(align == 0 ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Painter::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextFormat* f = Format(FamilyFor(size), size, weight, false);
    IDWriteTextLayout* l = nullptr;
    if (!f || FAILED(DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 4000, 200, &l))) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    Release(l);
    return m.widthIncludingTrailingWhitespace;
}

void Painter::Glyph(wchar_t g, float size, D2D1_RECT_F r, D2D1_COLOR_F c, float rotation) {
    IDWriteTextFormat* f = Format(IconFont(), size, DWRITE_FONT_WEIGHT_NORMAL, false);
    if (!f) return;
    f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1::Matrix3x2F base = D2D1::Matrix3x2F::Scale(scale, scale);
    if (rotation != 0)
        rt_->SetTransform(D2D1::Matrix3x2F::Rotation(rotation, D2D1::Point2F((r.left + r.right) / 2, (r.top + r.bottom) / 2)) *
                          base);
    wchar_t s[2] = { g, 0 };
    rt_->DrawTextW(s, 1, f, r, Brush(c));
    if (rotation != 0) rt_->SetTransform(base);
}

void Painter::Ellipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c) {
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), Brush(c));
}

namespace {
ID2D1PathGeometry* Path(const D2D1_POINT_2F* pts, size_t count, bool closed) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (count < 2 || !Factory() || FAILED(Factory()->CreatePathGeometry(&g))) return nullptr;
    if (FAILED(g->Open(&sink))) { Release(g); return nullptr; }
    sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddLines(pts + 1, (UINT32)(count - 1));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    Release(sink);
    return g;
}
}  // namespace

void Painter::Polygon(const D2D1_POINT_2F* pts, size_t count, D2D1_COLOR_F fill) {
    ID2D1PathGeometry* g = Path(pts, count, true);
    if (!g) return;
    rt_->FillGeometry(g, Brush(fill));
    Release(g);
}

void Painter::Outline(const D2D1_POINT_2F* pts, size_t count, D2D1_COLOR_F c, float width) {
    ID2D1PathGeometry* g = Path(pts, count, true);
    if (!g) return;
    rt_->DrawGeometry(g, Brush(c), width);
    Release(g);
}

namespace {
ID2D1GradientStopCollection* Stops(ID2D1RenderTarget* rt, D2D1_COLOR_F c0, D2D1_COLOR_F c1) {
    D2D1_GRADIENT_STOP stops[2];
    stops[0].position = 0;
    stops[0].color = c0;
    stops[1].position = 1;
    stops[1].color = c1;
    ID2D1GradientStopCollection* coll = nullptr;
    rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
    return coll;
}
}  // namespace

void Painter::PolygonGradient(const D2D1_POINT_2F* pts, size_t count, D2D1_POINT_2F from, D2D1_POINT_2F to, D2D1_COLOR_F c0,
                              D2D1_COLOR_F c1) {
    ID2D1PathGeometry* g = Path(pts, count, true);
    ID2D1GradientStopCollection* stops = g ? Stops(rt_, c0, c1) : nullptr;
    ID2D1LinearGradientBrush* brush = nullptr;
    if (stops) {
        D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props;
        props.startPoint = from;
        props.endPoint = to;
        rt_->CreateLinearGradientBrush(&props, nullptr, stops, &brush);
    }
    if (brush) {
        brush->SetOpacity(alpha);
        rt_->FillGeometry(g, brush);
    }
    Release(brush);
    Release(stops);
    Release(g);
}

void Painter::RadialEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F centre, D2D1_COLOR_F edge) {
    ID2D1GradientStopCollection* stops = Stops(rt_, centre, edge);
    ID2D1RadialGradientBrush* brush = nullptr;
    if (stops) {
        D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props;
        props.center = D2D1::Point2F(cx, cy);
        props.gradientOriginOffset = D2D1::Point2F(0, 0);
        props.radiusX = rx;
        props.radiusY = ry;
        rt_->CreateRadialGradientBrush(&props, nullptr, stops, &brush);
    }
    if (brush) {
        brush->SetOpacity(alpha);
        rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush);
    }
    Release(brush);
    Release(stops);
}

void Painter::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F c, float width, bool dotted) {
    static ID2D1StrokeStyle* dots = nullptr;
    if (dotted && !dots && Factory()) {
        float dashes[] = { 1, 2 };
        Factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                                 D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0),
                                     dashes, 2, &dots);
    }
    rt_->DrawLine(a, b, Brush(c), width, dotted ? dots : nullptr);
}

void Painter::Polyline(const D2D1_POINT_2F* pts, size_t count, D2D1_COLOR_F c, float width) {
    static ID2D1StrokeStyle* round = nullptr;
    if (!round && Factory())
        Factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                 D2D1_LINE_JOIN_ROUND),
                                     nullptr, 0, &round);
    ID2D1PathGeometry* g = Path(pts, count, false);
    if (!g) return;
    rt_->DrawGeometry(g, Brush(c), width, round);
    Release(g);
}

void Painter::Arc(D2D1_POINT_2F from, D2D1_POINT_2F to, float radius, D2D1_COLOR_F c, float width) {
    ID2D1PathGeometry* g = nullptr;
    ID2D1GeometrySink* sink = nullptr;
    if (!Factory() || FAILED(Factory()->CreatePathGeometry(&g))) return;
    if (SUCCEEDED(g->Open(&sink))) {
        sink->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddArc(D2D1::ArcSegment(to, D2D1::SizeF(radius, radius), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        Release(sink);
        ID2D1StrokeStyle* round = nullptr;
        Factory()->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
        rt_->DrawGeometry(g, Brush(c), width, round);
        Release(round);
    }
    Release(g);
}

void Painter::Transform(const D2D1_MATRIX_3X2_F& m) {
    rt_->SetTransform(D2D1::Matrix3x2F(m._11, m._12, m._21, m._22, m._31, m._32) * D2D1::Matrix3x2F::Scale(scale, scale));
}

void Painter::ResetTransform() { rt_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)); }

void Painter::Clip(D2D1_RECT_F r) { rt_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
void Painter::Unclip() { rt_->PopAxisAlignedClip(); }

// ---- controls ---------------------------------------------------------------------------------

void ToggleSwitch(Painter& p, const Palette& c, float right, float cy, bool on, Look look) {
    D2D1_RECT_F r{ right - 40, cy - 10, right, cy + 10 };
    float knob = look == Look::Hover ? 7.f : look == Look::Press ? 7.f : 6.f;  // 12 px knob, 14 px on hover
    if (on) {
        p.Fill(r, 10, look == Look::Press ? c.accentPressed : look == Look::Hover ? c.accentHover : c.accent);
        p.Circle(r.right - 10, cy, knob, c.onAccent);
    } else {
        p.Fill(r, 10, look == Look::Hover ? c.subtleHover : look == Look::Press ? c.subtlePressed : D2D1::ColorF(0, 0, 0, 0));
        p.Stroke(r, 10, c.controlStrongStroke);
        p.Circle(r.left + 10, cy, knob - 1, c.textSecondary);
    }
}

void Button(Painter& p, const Palette& c, D2D1_RECT_F r, const std::wstring& label, Look look, bool accent) {
    if (accent) {
        p.Fill(r, 4, look == Look::Press ? c.accentPressed : look == Look::Hover ? c.accentHover : c.accent);
        p.Text(label, kBody, DWRITE_FONT_WEIGHT_NORMAL, r, c.onAccent, 1);
        return;
    }
    p.Fill(r, 4, look == Look::Press ? c.controlPressed : look == Look::Hover ? c.controlHover : c.control);
    p.Stroke(r, 4, c.controlStroke);
    p.Text(label, kBody, DWRITE_FONT_WEIGHT_NORMAL, r, look == Look::Press ? c.textSecondary : c.text, 1);
}

void ComboBox(Painter& p, const Palette& c, D2D1_RECT_F r, const std::wstring& value, Look look) {
    p.Fill(r, 4, look == Look::Press ? c.controlPressed : look == Look::Hover ? c.controlHover : c.control);
    p.Stroke(r, 4, c.controlStroke);
    p.Text(value, kBody, DWRITE_FONT_WEIGHT_NORMAL, { r.left + 11, r.top, r.right - 36, r.bottom }, c.text);
    p.Glyph(glyph::ChevronDown, 12, { r.right - 32, r.top, r.right - 12, r.bottom }, c.textSecondary);
}

float ComboWidth(Painter& p, const std::wstring& longest) {
    return std::max(120.f, std::ceil(p.TextWidth(longest, kBody, DWRITE_FONT_WEIGHT_NORMAL)) + 11 + 40);
}

void IconButton(Painter& p, const Palette& c, D2D1_RECT_F r, wchar_t g, float glyphSize, Look look, float rotation) {
    if (look == Look::Hover || look == Look::Press) p.Fill(r, 4, look == Look::Press ? c.subtlePressed : c.subtleHover);
    p.Glyph(g, glyphSize, r, c.text, rotation);
}

// ---- backdrop ---------------------------------------------------------------------------------

namespace {
struct AccentPolicy { int state; int flags; DWORD color; int animation; };
struct CompositionData { int attribute; void* data; SIZE_T size; };
using SetCompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);

// The lighter blur behind the window (undocumented SetWindowCompositionAttribute, used by many taskbar
// tools), with a thin tint so text stays readable. state 4 = acrylic blur behind, 0 = off.
void SetGlass(HWND wnd, bool on, bool light, bool frosted) {
    static auto fn = (SetCompositionFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    DWORD tint = frosted ? (light ? 0xB8F3F3F3 : 0xA6201E1C) : (light ? 0x55F3F3F3 : 0x40201E1C);  // AABBGGRR
    AccentPolicy a{ on ? 4 : 0, 2, tint, 0 };
    CompositionData d{ 19, &a, sizeof(a) };  // 19 = WCA_ACCENT_POLICY
    fn(wnd, &d);
}
}  // namespace

void ApplyBackdrop(HWND wnd, bool light, Backdrop backdrop, bool activate) {
    BOOL dark = light ? FALSE : TRUE;
    DwmSetWindowAttribute(wnd, kDwmDarkMode, &dark, sizeof(dark));
    int corners = 2;  // round
    DwmSetWindowAttribute(wnd, kDwmCorners, &corners, sizeof(corners));
    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(wnd, &m);
    int type = backdrop == Backdrop::Mica ? 2 : backdrop == Backdrop::Acrylic ? 3 : 1;
    DwmSetWindowAttribute(wnd, kDwmBackdrop, &type, sizeof(type));
    SetGlass(wnd, backdrop == Backdrop::Glass || backdrop == Backdrop::Frosted, light, backdrop == Backdrop::Frosted);
    SetWindowPos(wnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | (activate ? 0 : SWP_NOACTIVATE));
}

bool BackdropSupported(HWND wnd) {
    int type = 2;
    return SUCCEEDED(DwmSetWindowAttribute(wnd, kDwmBackdrop, &type, sizeof(type)));
}

UINT WindowDpi(HWND wnd) {
    using GetDpiFn = UINT(WINAPI*)(HWND);
    static auto fn = (GetDpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    return fn ? fn(wnd) : 96;
}

bool AnimationsEnabled() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

}  // namespace fluent
