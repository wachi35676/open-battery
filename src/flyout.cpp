#include "flyout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

using fluent::Look;

// Panel layout in DIPs. Width and footer match Windows 11's Quick Settings (360 wide, 48 px footer).
constexpr float kWidth = 360, kPadX = 24, kTopLabels = 58, kScene = 214, kBottomLabels = 56, kRowH = 44, kFooterH = 48;
constexpr size_t kMaxRows = 4;

// Opening: the window rises from the taskbar, the leader lines grow out of the devices, then the levels
// fade in. All of it under 350 ms, and none of it when Windows animations are off.
constexpr float kRiseMs = 200, kLineMs = 260, kLabelDelayMs = 110, kLabelMs = 200, kRiseDip = 12;

// ---- isometric desk ---------------------------------------------------------------------------
// World units: the desk top spans x 0..10 (left to right) and y 0..4 (back to front) at z = 0.
constexpr float kCos30 = 0.8660254f, kUnit = 22;

struct Iso {
    float ox = 0, oy = 0, s = kUnit;
    D2D1_POINT_2F P(float x, float y, float z) const { return { ox + (x - y) * kCos30 * s, oy + (x + y) * 0.5f * s - z * s }; }
    D2D1_MATRIX_3X2_F Plane(float z) const { return D2D1::Matrix3x2F(kCos30 * s, 0.5f * s, -kCos30 * s, 0.5f * s, ox, oy - z * s); }
};

struct Materials {
    D2D1_COLOR_F deskTop, deskFront, deskSide, top, front, side, screen, detail, edge, shadow;
};

Materials SceneColors(bool light) {
    auto c = [](float r, float g, float b, float a) { return D2D1::ColorF(r, g, b, a); };
    if (light)
        return { c(0, 0, 0, .045f), c(0, 0, 0, .10f), c(0, 0, 0, .075f), c(1, 1, 1, .96f), c(.84f, .85f, .87f, 1),
                 c(.91f, .92f, .93f, 1), c(.13f, .14f, .16f, .94f), c(0, 0, 0, .09f), c(0, 0, 0, .10f), c(0, 0, 0, .08f) };
    return { c(1, 1, 1, .075f), c(1, 1, 1, .035f), c(1, 1, 1, .055f), c(1, 1, 1, .21f), c(1, 1, 1, .10f),
             c(1, 1, 1, .15f), c(0, 0, 0, .55f), c(1, 1, 1, .12f), c(1, 1, 1, .07f), c(0, 0, 0, .22f) };
}

HWND g_wnd;
fluent::Painter g_paint;
flyout::Model g_model;
flyout::Handler g_handler;
RECT g_anchor{};
int g_finalX = 0, g_finalY = 0, g_riseDir = 1;  // riseDir: +1 rises up (taskbar below), -1 drops down, 0 none
bool g_motion = true;
ULONGLONG g_openAt = 0, g_closedAt = 0, g_spinAt = 0;
bool g_timer = false;
int g_stage = 99;  // build stage (WB_FLYOUT_STAGE): 0 backdrop, 1 desk, 2 devices, 3 labels, 4 footer, 5 animation

enum class HitKind { Settings, Refresh, Device };
struct Hit {
    D2D1_RECT_F r;
    HitKind kind;
    std::wstring key;
};
std::vector<Hit> g_hits;
POINT g_mouse{ -1, -1 };
bool g_down = false;
int g_pressed = -1;

bool Inside(const D2D1_RECT_F& r) {
    float x = g_mouse.x / g_paint.scale, y = g_mouse.y / g_paint.scale;
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
Look LookOf(const D2D1_RECT_F& r) { return !Inside(r) ? Look::Rest : g_down ? Look::Press : Look::Hover; }

float Ease(float t) {  // ease-out cubic
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
float Elapsed(ULONGLONG since) { return since ? (float)(GetTickCount64() - since) : 1e9f; }
bool Animated() { return g_motion && g_stage >= 5; }

// ---- which device sits where ------------------------------------------------------------------

enum Slot { SlotHeadset, SlotPhone, SlotGamepad, SlotKeyboard, SlotMouse, SlotCount };

struct Placement {
    int device[SlotCount];  // index into g_model.devices, or -1
    std::vector<int> rest;  // devices without a place on the desk
};

Placement Place() {
    Placement p;
    std::fill(std::begin(p.device), std::end(p.device), -1);
    for (size_t i = 0; i < g_model.devices.size(); i++) {
        int slot = -1;
        switch (g_model.devices[i].kind) {
            case DeviceKind::Headset: slot = SlotHeadset; break;
            case DeviceKind::Phone: slot = SlotPhone; break;
            case DeviceKind::Gamepad: slot = SlotGamepad; break;
            case DeviceKind::Keyboard: slot = SlotKeyboard; break;
            case DeviceKind::Mouse: slot = SlotMouse; break;
            default: break;
        }
        if (slot >= 0 && p.device[slot] < 0) p.device[slot] = (int)i;
        else p.rest.push_back((int)i);
    }
    return p;
}

size_t RowCount(const Placement& p) { return std::min(p.rest.size(), kMaxRows); }

float ContentHeight() {
    Placement p = Place();
    float h = kTopLabels + kScene + kBottomLabels;
    if (RowCount(p)) h += 8 + RowCount(p) * kRowH + 8;
    return h + kFooterH;
}

// ---- drawing the desk -------------------------------------------------------------------------

void Quad(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d, D2D1_COLOR_F col) {
    D2D1_POINT_2F pts[4] = { a, b, c, d };
    g_paint.Polygon(pts, 4, col);
}

// A box: its top face (optionally rounded, drawn on the isometric plane), the front face (y = y1) and the
// right face (x = x1), which are the faces this view shows.
void Box(const Iso& iso, float x0, float x1, float y0, float y1, float z0, float z1, const Materials& m, float radius = 0,
         const D2D1_COLOR_F* topOverride = nullptr) {
    Quad(iso.P(x0, y1, z0), iso.P(x1, y1, z0), iso.P(x1, y1, z1), iso.P(x0, y1, z1), m.front);
    Quad(iso.P(x1, y0, z0), iso.P(x1, y1, z0), iso.P(x1, y1, z1), iso.P(x1, y0, z1), m.side);
    g_paint.Transform(iso.Plane(z1));
    g_paint.Fill({ x0, y0, x1, y1 }, radius, topOverride ? *topOverride : m.top);
    g_paint.ResetTransform();
}

// Contact shadow on the desk under an object.
void Shadow(const Iso& iso, float x0, float x1, float y0, float y1, const Materials& m, float radius) {
    g_paint.Transform(iso.Plane(0));
    g_paint.Fill({ x0 + .08f, y0 + .12f, x1 + .18f, y1 + .2f }, radius, m.shadow);
    g_paint.ResetTransform();
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

// The top-face colour of a device: tinted with the accent while charging, red when low.
D2D1_COLOR_F DeviceTop(const Materials& m, const fluent::Palette& c, int index, bool hover) {
    D2D1_COLOR_F top = m.top;
    if (index >= 0) {
        const auto& d = g_model.devices[index];
        if (d.charging || d.full) top = Mix(top, c.accent, g_model.light ? .45f : .55f);
        else if (d.low) top = Mix(top, c.critical, .5f);
    }
    if (hover) top = Mix(top, g_model.light ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(1, 1, 1, .4f), .35f);
    return top;
}

struct Anchor {
    bool used = false, top = false;  // label above the scene (top) or below it
    D2D1_POINT_2F at{};              // where the leader line starts, on the device
    D2D1_RECT_F bounds{};            // the device's area on screen, for hover and clicks
};

void DrawDesk(const Iso& iso, const Materials& m, const fluent::Palette& c, const Placement& pl, Anchor* anchors,
              int hoverSlot) {
    // Desk slab.
    Quad(iso.P(0, 4, -.32f), iso.P(10, 4, -.32f), iso.P(10, 4, 0), iso.P(0, 4, 0), m.deskFront);
    Quad(iso.P(10, 0, -.32f), iso.P(10, 4, -.32f), iso.P(10, 4, 0), iso.P(10, 0, 0), m.deskSide);
    g_paint.Transform(iso.Plane(0));
    g_paint.Fill({ 0, 0, 10, 4 }, .12f, m.deskTop);
    g_paint.ResetTransform();
    if (g_stage < 2) return;

    // Monitor: base, neck, then the screen (its front face holds the display, faintly lit by the accent).
    Shadow(iso, 4.2f, 5.8f, .25f, .95f, m, .3f);
    g_paint.Transform(iso.Plane(.03f));
    g_paint.Fill({ 4.3f, .3f, 5.7f, .9f }, .25f, m.side);
    g_paint.ResetTransform();
    Box(iso, 4.86f, 5.14f, .18f, .32f, 0, 1.0f, m);
    Box(iso, 2.4f, 7.6f, .34f, .5f, .95f, 3.7f, m, 0);
    {
        float in = .12f;
        D2D1_POINT_2F a = iso.P(2.4f + in, .5f, .95f + in), b = iso.P(7.6f - in, .5f, .95f + in), cc = iso.P(7.6f - in, .5f, 3.7f - in),
                      d = iso.P(2.4f + in, .5f, 3.7f - in);
        Quad(a, b, cc, d, m.screen);
        D2D1_COLOR_F glow = c.accent;
        glow.a = g_model.light ? .16f : .22f;
        Quad(a, b, cc, d, glow);
    }

    // Headset on its stand (back left).
    int hs = pl.device[SlotHeadset];
    if (hs >= 0) {
        D2D1_COLOR_F top = DeviceTop(m, c, hs, hoverSlot == SlotHeadset);
        Shadow(iso, .85f, 1.75f, .6f, 1.4f, m, .45f);
        g_paint.Transform(iso.Plane(.04f));
        g_paint.Fill({ .85f, .6f, 1.75f, 1.4f }, .45f, m.side);
        g_paint.ResetTransform();
        Box(iso, 1.22f, 1.38f, .92f, 1.08f, 0, 2.25f, m);
        D2D1_POINT_2F t = iso.P(1.3f, 1.0f, 2.25f);
        g_paint.Arc({ t.x - 12, t.y + 9 }, { t.x + 12, t.y + 9 }, 12.5f, top, 4);
        g_paint.Fill({ t.x - 17, t.y + 6, t.x - 8, t.y + 22 }, 3.5f, top);
        g_paint.Fill({ t.x + 8, t.y + 6, t.x + 17, t.y + 22 }, 3.5f, top);
        anchors[SlotHeadset] = { true, true, { t.x, t.y - 6 }, { t.x - 20, t.y - 6, t.x + 20, t.y + 70 } };
    }

    // Phone lying on the desk (back right).
    int ph = pl.device[SlotPhone];
    if (ph >= 0) {
        Shadow(iso, 8.4f, 9.2f, .55f, 1.95f, m, .12f);
        D2D1_COLOR_F top = DeviceTop(m, c, ph, hoverSlot == SlotPhone);
        Box(iso, 8.4f, 9.2f, .55f, 1.95f, 0, .09f, m, .14f, &top);
        g_paint.Transform(iso.Plane(.09f));
        g_paint.Fill({ 8.48f, .66f, 9.12f, 1.84f }, .1f, m.screen);
        g_paint.ResetTransform();
        D2D1_POINT_2F a = iso.P(8.8f, 1.25f, .1f);
        anchors[SlotPhone] = { true, true, a, { a.x - 22, a.y - 18, a.x + 22, a.y + 18 } };
    }

    // Controller (front left).
    int gp = pl.device[SlotGamepad];
    if (gp >= 0) {
        Shadow(iso, .6f, 2.2f, 2.35f, 3.25f, m, .4f);
        D2D1_COLOR_F top = DeviceTop(m, c, gp, hoverSlot == SlotGamepad);
        Box(iso, .6f, 2.2f, 2.35f, 3.25f, 0, .2f, m, .4f, &top);
        g_paint.Transform(iso.Plane(.2f));
        g_paint.Ellipse(1.05f, 2.75f, .14f, .14f, m.detail);
        g_paint.Ellipse(1.75f, 2.85f, .14f, .14f, m.detail);
        g_paint.ResetTransform();
        D2D1_POINT_2F a = iso.P(1.4f, 3.25f, 0);
        anchors[SlotGamepad] = { true, false, a, { a.x - 26, a.y - 22, a.x + 26, a.y + 6 } };
    }

    // Keyboard (front centre): always on the desk; a battery level only when it is a wireless one.
    {
        int kb = pl.device[SlotKeyboard];
        Shadow(iso, 2.8f, 7.0f, 2.1f, 3.3f, m, .12f);
        D2D1_COLOR_F top = DeviceTop(m, c, kb, kb >= 0 && hoverSlot == SlotKeyboard);
        Box(iso, 2.8f, 7.0f, 2.1f, 3.3f, 0, .16f, m, .12f, &top);
        g_paint.Transform(iso.Plane(.16f));
        for (int r = 1; r < 5; r++) g_paint.Rect({ 2.95f, 2.1f + r * .24f - .012f, 6.85f, 2.1f + r * .24f + .012f }, m.detail);
        for (float x = 3.3f; x < 6.8f; x += .38f) g_paint.Rect({ x - .012f, 2.22f, x + .012f, 3.18f }, m.detail);
        g_paint.ResetTransform();
        if (kb >= 0) {
            D2D1_POINT_2F a = iso.P(4.9f, 3.3f, 0);
            anchors[SlotKeyboard] = { true, false, a, { a.x - 60, a.y - 40, a.x + 50, a.y + 4 } };
        }
    }

    // Mouse (front right).
    int ms = pl.device[SlotMouse];
    if (ms >= 0) {
        Shadow(iso, 7.75f, 8.45f, 2.25f, 3.25f, m, .35f);
        D2D1_COLOR_F top = DeviceTop(m, c, ms, hoverSlot == SlotMouse);
        Box(iso, 7.75f, 8.45f, 2.25f, 3.25f, 0, .22f, m, .35f, &top);
        g_paint.Transform(iso.Plane(.22f));
        g_paint.Rect({ 8.09f, 2.3f, 8.11f, 2.7f }, m.detail);
        g_paint.ResetTransform();
        D2D1_POINT_2F a = iso.P(8.1f, 3.25f, 0);
        anchors[SlotMouse] = { true, false, a, { a.x - 18, a.y - 26, a.x + 18, a.y + 4 } };
    }
}

// ---- labels -----------------------------------------------------------------------------------

std::wstring SlotName(int slot) {
    switch (slot) {
        case SlotHeadset: return g_model.text.headset;
        case SlotPhone: return g_model.text.phone;
        case SlotGamepad: return g_model.text.gamepad;
        case SlotKeyboard: return g_model.text.keyboard;
        default: return g_model.text.mouse;
    }
}

// Label rect for hit testing, matching DrawLabel's placement.
D2D1_RECT_F LabelRect(const Anchor& a, float sceneTop, float sceneBottom) {
    float top = a.top ? sceneTop - 52 : sceneBottom + 4;
    float x = a.at.x + 90 > kWidth - 12 ? a.at.x - 96 : a.at.x - 6;
    return { x, top, x + 96, top + 48 };
}

// A level with its leader line: "92%" in Subtitle (20 px semibold) and the kind in Caption underneath,
// left-aligned 8 px right of the line, as in a product diagram.
void DrawLabel(const fluent::Palette& c, const flyout::Device& d, const Anchor& a, int slot, float sceneTop, float sceneBottom,
               bool hover) {
    float lineT = Animated() ? Ease(Elapsed(g_openAt) / kLineMs) : 1;
    float labelA = Animated() ? Ease((Elapsed(g_openAt) - kLabelDelayMs) / kLabelMs) : 1;
    float endY = a.top ? sceneTop - 2 : sceneBottom + 2;
    float y = a.at.y + (endY - a.at.y) * lineT;
    D2D1_COLOR_F lineC = hover ? c.textSecondary : c.textTertiary;
    g_paint.Line(a.at, { a.at.x, y }, lineC, 1, true);
    g_paint.Circle(a.at.x, a.at.y, 2, lineC);

    float keep = g_paint.alpha;
    g_paint.alpha *= labelA;
    std::wstring value = !d.levelText.empty() ? d.levelText : d.percent >= 0 ? std::to_wstring(d.percent) : L"--";
    bool pct = d.levelText.empty() && d.percent >= 0;
    D2D1_COLOR_F vc = d.dim ? c.textTertiary : d.low ? c.critical : c.text;
    float tw = g_paint.TextWidth(value, fluent::kSubtitle, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    float vw = tw + (pct ? 12 : 0) + ((d.charging || d.full) ? 16 : 0);
    float nw = g_paint.TextWidth(SlotName(slot), fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL);
    float w = std::max(vw, nw);
    float x = a.at.x + 8;
    if (x + w > kWidth - 12) x = a.at.x - 8 - w;  // near the right edge: to the left of the line
    float top = a.top ? sceneTop - 50 : sceneBottom + 6;
    g_paint.Text(value, fluent::kSubtitle, DWRITE_FONT_WEIGHT_SEMI_BOLD, { x, top, x + 120, top + 28 }, vc);
    float after = x + tw + 1;
    if (pct) {
        g_paint.Text(L"%", fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL, { after, top + 9, after + 14, top + 25 }, c.textSecondary);
        after += 12;
    }
    if (d.charging || d.full)
        g_paint.Glyph(d.full ? fluent::glyph::Check : fluent::glyph::Bolt, 12, { after + 2, top + 6, after + 16, top + 24 }, c.accent);
    g_paint.Text(SlotName(slot), fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL, { x, top + 27, x + 140, top + 43 }, c.textSecondary);
    g_paint.alpha = keep;
}

void DrawRow(const fluent::Palette& c, const flyout::Device& d, D2D1_RECT_F r) {
    g_hits.push_back({ r, HitKind::Device, d.key });
    Look look = LookOf(r);
    if (look != Look::Rest) g_paint.Fill(r, 4, look == Look::Press ? c.subtlePressed : c.subtleHover);
    float cy = (r.top + r.bottom) / 2;
    D2D1_COLOR_F main = d.dim ? c.textTertiary : c.text;
    g_paint.Glyph(fluent::DeviceGlyph(d.kind, d.bluetooth), 16, { r.left + 4, cy - 12, r.left + 28, cy + 12 }, main);
    std::wstring level = !d.levelText.empty() ? d.levelText : d.percent >= 0 ? std::to_wstring(d.percent) + L"%" : L"";
    float lw = level.empty() ? 0 : g_paint.TextWidth(level, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL);
    g_paint.Text(level, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { r.right - 8 - lw - 2, cy - 10, r.right - 8, cy + 10 },
                 d.low ? c.critical : main, 2);
    float gx = r.right - 8 - lw - 28;
    g_paint.Glyph(fluent::BatteryGlyph(d.percent, d.charging || d.full), 16, { gx, cy - 10, gx + 22, cy + 10 }, d.low ? c.critical : main);
    g_paint.Text(d.name, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { r.left + 40, cy - 10, gx - 8, cy + 10 }, main);
}

void Draw() {
    if (!g_paint.Begin(g_wnd)) return;
    fluent::Palette c = fluent::Colors(g_model.light);
    Materials m = SceneColors(g_model.light);
    g_hits.clear();
    float total = ContentHeight(), body = total - kFooterH;

    Placement pl = Place();
    float sceneTop = kTopLabels, sceneBottom = kTopLabels + kScene;
    // Centre the desk: its projection spans x from -3.46 to 8.66 units and y from about -2.33 to 7.3 units.
    Iso iso;
    iso.ox = kWidth / 2 - (8.66f - 3.46f) / 2 * kUnit;
    iso.oy = sceneTop + 2.45f * kUnit;

    Anchor anchors[SlotCount];
    int hoverSlot = -1;
    if (g_stage >= 1) {
        // Anchors come from a first pass; the hovered device is then drawn highlighted.
        DrawDesk(iso, m, c, pl, anchors, -1);
        for (int s = 0; s < SlotCount; s++) {
            if (!anchors[s].used || pl.device[s] < 0) continue;
            D2D1_RECT_F label = LabelRect(anchors[s], sceneTop, sceneBottom);
            if (Inside(anchors[s].bounds) || Inside(label)) hoverSlot = s;
            g_hits.push_back({ anchors[s].bounds, HitKind::Device, g_model.devices[pl.device[s]].key });
            g_hits.push_back({ label, HitKind::Device, g_model.devices[pl.device[s]].key });
        }
        if (hoverSlot >= 0) DrawDesk(iso, m, c, pl, anchors, hoverSlot);
    }

    if (g_stage >= 3)
        for (int s = 0; s < SlotCount; s++)
            if (anchors[s].used && pl.device[s] >= 0)
                DrawLabel(c, g_model.devices[pl.device[s]], anchors[s], s, sceneTop, sceneBottom, hoverSlot == s);

    float y = sceneBottom + kBottomLabels;
    if (g_stage >= 3 && g_model.devices.empty()) {
        g_paint.Text(g_model.text.emptyTitle, fluent::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, { kPadX, 10, kWidth - kPadX, 30 }, c.text, 1);
        g_paint.Text(g_model.text.emptyBody, fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL, { kPadX, 30, kWidth - kPadX, 48 },
                     c.textSecondary, 1);
    }
    if (g_stage >= 3 && RowCount(pl)) {
        y += 8;
        for (size_t i = 0; i < RowCount(pl); i++)
            DrawRow(c, g_model.devices[pl.rest[i]], { kPadX - 8, y + i * kRowH, kWidth - kPadX + 8, y + (i + 1) * kRowH });
    }

    if (g_stage >= 4) {
        g_paint.Rect({ 0, body, kWidth, body + 1 }, c.divider);
        float cy = body + kFooterH / 2;
        g_paint.Text(g_model.text.summary, fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL, { kPadX, body, kWidth - 110, body + kFooterH },
                     c.textSecondary);
        float spin = Animated() && Elapsed(g_spinAt) < 500 ? Ease(Elapsed(g_spinAt) / 500) * 360 : 0;
        // Icon buttons centred 30 px (settings) and 70 px (refresh) from the right edge, 14 px glyphs as in
        // Quick Settings' footer.
        D2D1_RECT_F refresh{ kWidth - 88, cy - 16, kWidth - 52, cy + 16 }, settings{ kWidth - 48, cy - 16, kWidth - 12, cy + 16 };
        fluent::IconButton(g_paint, c, refresh, fluent::glyph::Refresh, 14, LookOf(refresh), spin);
        fluent::IconButton(g_paint, c, settings, fluent::glyph::Settings, 14, LookOf(settings));
        g_hits.push_back({ refresh, HitKind::Refresh });
        g_hits.push_back({ settings, HitKind::Settings });
    }
    g_paint.End();
}

// ---- window -----------------------------------------------------------------------------------

void StartTimer() {
    if (!g_timer && g_wnd) {
        SetTimer(g_wnd, 1, 10, nullptr);
        g_timer = true;
    }
}

bool Animating() {
    if (!Animated()) return false;
    return Elapsed(g_openAt) < kLabelDelayMs + kLabelMs + 20 || Elapsed(g_spinAt) < 500;
}

void ApplyWindowPos() {
    float rise = Animated() ? (1 - Ease(Elapsed(g_openAt) / kRiseMs)) * kRiseDip * g_paint.scale : 0;
    SetWindowPos(g_wnd, HWND_TOPMOST, g_finalX, g_finalY + (int)std::lround(rise) * g_riseDir, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void ApplyBackdrop() { fluent::ApplyBackdrop(g_wnd, g_model.light, g_model.backdrop); }

// Size and place the window next to the anchor, inside the work area of the anchor's monitor.
void PlaceWindow() {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&g_anchor, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(g_wnd, nullptr, mi.rcWork.left, mi.rcWork.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_paint.scale = fluent::WindowDpi(g_wnd) / 96.f;  // after moving there: that monitor's DPI
    int w = (int)std::lround(kWidth * g_paint.scale), h = (int)std::lround(ContentHeight() * g_paint.scale);
    int gap = (int)std::lround(12 * g_paint.scale);
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
    g_paint.Resize(w, h);
}

void Activate(const Hit& hit) {
    flyout::Handler handler = g_handler;
    switch (hit.kind) {
        case HitKind::Settings: flyout::Hide(); if (handler) handler(flyout::Command::Settings, L""); break;
        case HitKind::Device: flyout::Hide(); if (handler) handler(flyout::Command::History, hit.key); break;
        case HitKind::Refresh:
            g_spinAt = GetTickCount64();
            StartTimer();
            if (handler) handler(flyout::Command::Refresh, L"");
            break;
    }
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

int HitAt(POINT pt) {
    float x = pt.x / g_paint.scale, y = pt.y / g_paint.scale;
    for (size_t i = g_hits.size(); i-- > 0;) {
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
            g_mouse = { (short)LOWORD(lp), (short)HIWORD(lp) };
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
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
            if (hit >= 0 && pressed >= 0 && pressed < (int)g_hits.size() && g_hits[hit].kind == g_hits[pressed].kind &&
                g_hits[hit].key == g_hits[pressed].key) {
                Hit copy = g_hits[hit];
                Activate(copy);
            } else {
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED: return 0;  // placed by PlaceWindow() for the anchor's monitor
        case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool Init(HINSTANCE inst) {
    if (g_wnd) return true;
    if (const char* s = getenv("WB_FLYOUT_STAGE")) g_stage = atoi(s);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"WirelessBatteryFlyout";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"BatteryHub", WS_POPUP, 0, 0, 10, 10, nullptr,
                            nullptr, inst, nullptr);
    if (!g_wnd) return false;
    if (!fluent::BackdropSupported(g_wnd)) {  // before Windows 11 22H2
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
    g_motion = fluent::AnimationsEnabled();
    g_mouse = { -1, -1 };
    g_down = false;
    g_pressed = -1;
    g_spinAt = 0;
    ApplyBackdrop();
    PlaceWindow();
    g_openAt = GetTickCount64();
    ApplyWindowPos();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    InvalidateRect(g_wnd, nullptr, FALSE);
    UpdateWindow(g_wnd);
    ApplyBackdrop();
    if (Animated()) StartTimer();
    return true;
}

void Update(const Model& model) {
    if (!Visible()) return;
    bool theme = model.light != g_model.light || model.backdrop != g_model.backdrop;
    g_model = model;
    if (theme) ApplyBackdrop();
    PlaceWindow();  // the list under the desk may have grown or shrunk
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
