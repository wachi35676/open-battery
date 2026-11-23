#include "flyout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

using fluent::Look;

// Panel layout in DIPs. Width and footer match Windows 11's Quick Settings (360 wide, 48 px footer).
constexpr float kWidth = 360, kPadX = 24, kTopLabels = 56, kScene = 206, kBottomLabels = 56, kRowH = 44, kFooterH = 48;
constexpr size_t kMaxRows = 4;

// Opening: the window rises from the taskbar, the leader lines grow out of the devices, then the levels
// fade in. All of it under 350 ms, and none of it when Windows animations are off.
constexpr float kRiseMs = 200, kLineMs = 260, kLabelDelayMs = 110, kLabelMs = 200, kRiseDip = 12;

// Charging: a soft light runs along the cable to the device, once per period, redrawn at about 30 fps for as
// long as the panel is open and something on the desk is charging. Without animations the cable just glows.
constexpr float kPulsePeriodMs = 1800, kPulseTravelMs = 1100;
constexpr UINT_PTR kOpenTimer = 1, kChargeTimer = 2;
constexpr UINT kChargeFrameMs = 33;

// ---- isometric desk ---------------------------------------------------------------------------
// World units: the desk top spans x 0..10 (left to right) and y 0..4 (back to front) at z = 0.
constexpr float kCos30 = 0.8660254f, kUnit = 22;

struct Iso {
    float ox = 0, oy = 0, s = kUnit;
    D2D1_POINT_2F P(float x, float y, float z) const { return { ox + (x - y) * kCos30 * s, oy + (x + y) * 0.5f * s - z * s }; }
    D2D1_MATRIX_3X2_F Plane(float z) const { return D2D1::Matrix3x2F(kCos30 * s, 0.5f * s, -kCos30 * s, 0.5f * s, ox, oy - z * s); }
};

// The three faces of a solid that this view shows. The light comes from the upper left, so the top is the
// lightest, the right side (x = x1) a step darker and the front (y = y1) the darkest.
struct Shades {
    D2D1_COLOR_F top, side, front;
};

// Every object face is an opaque solid; only shadows, glows and edge highlights are translucent.
struct Materials {
    Shades desk, body;                 // the desk slab; devices, stands and the monitor
    D2D1_COLOR_F mat, screen, cable;   // desk mat; an unlit display; a charging cable
    D2D1_COLOR_F rim, shadow, ground;  // top-edge highlight, contact shadow, the shadow under the desk
    float sideK, frontK;               // how much darker the side and the front are than the top
};

D2D1_COLOR_F Shade(D2D1_COLOR_F c, float k) { return D2D1::ColorF(c.r * k, c.g * k, c.b * k, 1); }

Shades Solid(D2D1_COLOR_F top, const Materials& m) {
    top.a = 1;
    return { top, Shade(top, m.sideK), Shade(top, m.frontK) };
}

Materials SceneColors(bool light) {
    Materials m{};
    if (light) {
        m.sideK = .87f, m.frontK = .76f;
        m.desk = Solid(D2D1::ColorF(0xDDDFE4), m);
        m.body = Solid(D2D1::ColorF(0xFBFBFC), m);
        m.mat = D2D1::ColorF(0xCDD0D6);
        m.screen = D2D1::ColorF(0x1B1C20);
        m.cable = D2D1::ColorF(0x8D9097);
        m.rim = D2D1::ColorF(1, 1, 1, .75f);
        m.shadow = D2D1::ColorF(0, 0, 0, .15f);
        m.ground = D2D1::ColorF(0, 0, 0, .15f);
    } else {
        m.sideK = .78f, m.frontK = .62f;
        m.desk = Solid(D2D1::ColorF(0x3A3A3D), m);
        m.body = Solid(D2D1::ColorF(0x5C5C61), m);
        m.mat = D2D1::ColorF(0x2A2A2D);
        m.screen = D2D1::ColorF(0x0E0F11);
        m.cable = D2D1::ColorF(0x7E7E84);
        m.rim = D2D1::ColorF(1, 1, 1, .13f);
        m.shadow = D2D1::ColorF(0, 0, 0, .5f);
        m.ground = D2D1::ColorF(0, 0, 0, .6f);
    }
    return m;
}

HWND g_wnd;
fluent::Painter g_paint;
flyout::Model g_model;
flyout::Handler g_handler;
RECT g_anchor{};
int g_finalX = 0, g_finalY = 0, g_riseDir = 1;  // riseDir: +1 rises up (taskbar below), -1 drops down, 0 none
bool g_motion = true;
ULONGLONG g_openAt = 0, g_closedAt = 0, g_spinAt = 0;
bool g_timer = false, g_chargeTimer = false;
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

constexpr float kPi = 3.14159265f;
using Outline = std::vector<D2D1_POINT_2F>;  // a convex shape on the desk plane, in world x / y

void Quad(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_POINT_2F d, D2D1_COLOR_F col) {
    D2D1_POINT_2F pts[4] = { a, b, c, d };
    g_paint.Polygon(pts, 4, col);
}

D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

Outline RoundRect(float x0, float x1, float y0, float y1, float r) {
    Outline o;
    if (r <= 0) {
        o = { { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } };
        return o;
    }
    const float cx[4] = { x1 - r, x1 - r, x0 + r, x0 + r }, cy[4] = { y0 + r, y1 - r, y1 - r, y0 + r };
    const int n = 6;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= n; i++) {
            float a = ((k - 1) * 90.f + i * 90.f / n) * kPi / 180;
            o.push_back({ cx[k] + r * std::cos(a), cy[k] + r * std::sin(a) });
        }
    return o;
}

Outline Oval(float cx, float cy, float rx, float ry, int n = 28) {
    Outline o;
    for (int i = 0; i < n; i++) {
        float a = i * 2 * kPi / n;
        o.push_back({ cx + rx * std::cos(a), cy + ry * std::sin(a) });
    }
    return o;
}

Outline Project(const Iso& iso, const Outline& o, float z) {
    Outline p;
    p.reserve(o.size());
    for (const auto& q : o) p.push_back(iso.P(q.x, q.y, z));
    return p;
}

// The walls of `o` extruded from z0 to z1: each wall segment that faces the viewer, shaded by where it
// faces (front, right side, or in between on a rounded corner).
void Walls(const Iso& iso, const Outline& o, float z0, float z1, const Shades& s) {
    size_t n = o.size();
    float area = 0;
    for (size_t i = 0; i < n; i++) {
        const auto &a = o[i], &b = o[(i + 1) % n];
        area += a.x * b.y - b.x * a.y;
    }
    float sign = area > 0 ? 1.f : -1.f;
    // An underlay in the in-between shade, so the joins between wall segments never show what is behind.
    int steps = std::max(1, (int)std::ceil((z1 - z0) * iso.s / 1.5f));
    D2D1_COLOR_F mid = Mix(s.side, s.front, .5f);
    for (int k = 0; k <= steps; k++) {
        Outline p = Project(iso, o, z0 + (z1 - z0) * k / steps);
        g_paint.Polygon(p.data(), p.size(), mid);
    }
    struct Wall {
        size_t i;
        float depth, w;
    };
    std::vector<Wall> walls;
    for (size_t i = 0; i < n; i++) {
        const auto &a = o[i], &b = o[(i + 1) % n];
        float nx = (b.y - a.y) * sign, ny = -(b.x - a.x) * sign;  // outward normal
        if (nx + ny <= 0) continue;                                // faces away
        walls.push_back({ i, a.x + a.y + b.x + b.y, std::clamp(std::atan2(ny, nx) / (kPi / 2), 0.f, 1.f) });
    }
    // Back to front, which matters for an outline with a notch in it.
    std::sort(walls.begin(), walls.end(), [](const Wall& p, const Wall& q) { return p.depth < q.depth; });
    for (const Wall& wl : walls) {
        const auto &a = o[wl.i], &b = o[(wl.i + 1) % n];
        Quad(iso.P(a.x, a.y, z0), iso.P(b.x, b.y, z0), iso.P(b.x, b.y, z1), iso.P(a.x, a.y, z1), Mix(s.side, s.front, wl.w));
    }
}

// A closed smooth curve through `ctrl` (Catmull-Rom), `per` points for each span.
Outline Smooth(const Outline& ctrl, int per) {
    Outline o;
    size_t n = ctrl.size();
    for (size_t i = 0; i < n; i++) {
        const auto &p0 = ctrl[(i + n - 1) % n], &p1 = ctrl[i], &p2 = ctrl[(i + 1) % n], &p3 = ctrl[(i + 2) % n];
        for (int k = 0; k < per; k++) {
            float t = (float)k / per, t2 = t * t, t3 = t2 * t;
            o.push_back({ .5f * (2 * p1.x + (p2.x - p0.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (3 * p1.x - p0.x - 3 * p2.x + p3.x) * t3),
                          .5f * (2 * p1.y + (p2.y - p0.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (3 * p1.y - p0.y - 3 * p2.y + p3.y) * t3) });
        }
    }
    return o;
}

void Top(const Iso& iso, const Outline& o, float z, D2D1_COLOR_F col, const D2D1_COLOR_F* rim = nullptr) {
    Outline p = Project(iso, o, z);
    g_paint.Polygon(p.data(), p.size(), col);
    if (rim) g_paint.Outline(p.data(), p.size(), *rim, 1);
}

// A solid: walls, top face, and optionally a faint highlight along the top edges.
void Extrude(const Iso& iso, const Outline& o, float z0, float z1, const Shades& s, const D2D1_COLOR_F* rim = nullptr) {
    Walls(iso, o, z0, z1, s);
    Top(iso, o, z1, s.top, rim);
}

// Soft contact shadow on the desk under an object, pushed a little away from the light.
void Shadow(const Iso& iso, float x0, float x1, float y0, float y1, float radius, const Materials& m) {
    D2D1_COLOR_F c = m.shadow;
    c.a /= 4;
    g_paint.Transform(iso.Plane(0));
    for (int i = 0; i < 4; i++) {
        float g = .02f + i * .07f;
        g_paint.Fill({ x0 - g + .1f, y0 - g + .06f, x1 + g + .1f, y1 + g + .06f }, radius + g, c);
    }
    g_paint.ResetTransform();
}

// A short cylinder lying along x (an ear cup): its wall, lighter on top, and the round face at x1.
void CupX(const Iso& iso, float x0, float x1, float yc, float zc, float r, D2D1_COLOR_F wallTop, D2D1_COLOR_F wallBottom,
          D2D1_COLOR_F face) {
    auto disc = [&](float x) {
        Outline p;
        for (int i = 0; i < 24; i++) {
            float a = i * 2 * kPi / 24;
            p.push_back(iso.P(x, yc + r * std::cos(a), zc + r * std::sin(a)));
        }
        return p;
    };
    int steps = std::max(1, (int)std::ceil((x1 - x0) * iso.s));
    for (int k = 0; k <= steps; k++) {
        float x = x0 + (x1 - x0) * k / steps;
        Outline p = disc(x);
        g_paint.PolygonGradient(p.data(), p.size(), iso.P(x, yc, zc + r), iso.P(x, yc, zc - r), wallTop, wallBottom);
    }
    Outline p = disc(x1);
    g_paint.Polygon(p.data(), p.size(), face);
}

// The top-face colour of a device: tinted with the accent while charging, red when low.
D2D1_COLOR_F DeviceTop(const Materials& m, const fluent::Palette& c, int index, bool hover) {
    D2D1_COLOR_F top = m.body.top;
    if (index >= 0) {
        const auto& d = g_model.devices[index];
        if (d.charging || d.full) top = Mix(top, c.accent, g_model.light ? .2f : .26f);
        else if (d.low) top = Mix(top, c.critical, .5f);
    }
    if (hover) top = g_model.light ? Shade(top, .92f) : Mix(top, D2D1::ColorF(1, 1, 1, 1), .3f);
    top.a = 1;
    return top;
}

// A display: dark glass lit by `tint`, brighter towards `from`.
void Screen(const D2D1_POINT_2F* pts, size_t n, D2D1_POINT_2F from, D2D1_POINT_2F to, const Materials& m, D2D1_COLOR_F tint,
            float strength) {
    g_paint.PolygonGradient(pts, n, from, to, Mix(m.screen, tint, strength), Mix(m.screen, tint, strength * .28f));
}

struct Anchor {
    bool used = false, top = false;  // label above the scene (top) or below it
    D2D1_POINT_2F at{};              // where the leader line starts, on the device
    D2D1_RECT_F bounds{};            // the device's area on screen, for hover and clicks
};

// Where things stand on the desk (world units).
constexpr float kHeadX = 1.3f, kHeadY = 1.0f, kBandR = .56f, kBandZ = 1.7f;  // headset stand; headband arc
constexpr float kPhoneX0 = 8.5f, kPhoneX1 = 9.35f, kPhoneY0 = .5f, kPhoneY1 = 2.0f;
constexpr float kPadCx = 1.38f, kPadHalf = .95f, kPadY0 = 2.3f, kPadLen = 1.22f, kPadNotch = .57f;  // controller
constexpr float kStandX0 = 4.15f, kStandX1 = 5.85f, kStandY0 = .06f, kStandY1 = 1.0f;                // monitor foot
constexpr float kKeysX0 = 2.8f, kKeysX1 = 7.0f, kKeysY0 = 2.1f, kKeysY1 = 3.3f;
constexpr float kMouseX = 8.05f, kMouseY0 = 2.15f, kMouseY1 = 3.4f, kMouseW = .84f;

// Leader-line start points and hover / click areas of the devices that are on the desk.
void PlaceAnchors(const Iso& iso, const Placement& pl, Anchor* anchors) {
    if (g_stage < 2) return;
    if (pl.device[SlotHeadset] >= 0) {
        D2D1_POINT_2F t = iso.P(kHeadX, kHeadY, kBandZ + kBandR);
        anchors[SlotHeadset] = { true, true, { t.x, t.y - 1 }, { t.x - 24, t.y - 4, t.x + 26, t.y + 64 } };
    }
    if (pl.device[SlotPhone] >= 0) {
        D2D1_POINT_2F a = iso.P((kPhoneX0 + kPhoneX1) / 2, (kPhoneY0 + kPhoneY1) / 2, .08f);
        anchors[SlotPhone] = { true, true, a, { a.x - 24, a.y - 18, a.x + 24, a.y + 18 } };
    }
    if (pl.device[SlotGamepad] >= 0) {
        D2D1_POINT_2F a = iso.P(kPadCx, kPadY0 + kPadNotch * kPadLen, 0);
        anchors[SlotGamepad] = { true, false, a, { a.x - 30, a.y - 26, a.x + 30, a.y + 12 } };
    }
    if (pl.device[SlotKeyboard] >= 0) {
        D2D1_POINT_2F a = iso.P((kKeysX0 + kKeysX1) / 2 + .2f, kKeysY1, 0);
        anchors[SlotKeyboard] = { true, false, a, { a.x - 60, a.y - 40, a.x + 50, a.y + 4 } };
    }
    if (pl.device[SlotMouse] >= 0) {
        D2D1_POINT_2F a = iso.P(kMouseX + .2f, kMouseY1 - .18f, 0);
        anchors[SlotMouse] = { true, false, a, { a.x - 20, a.y - 30, a.x + 20, a.y + 4 } };
    }
}

// ---- charging cables --------------------------------------------------------------------------

struct P3 {
    float x, y, z;
};

bool Powered(int index) { return index >= 0 && (g_model.devices[index].charging || g_model.devices[index].full); }

// The cable of the device in `slot`: from its charging port, flat across the desk, to the back edge.
std::vector<P3> CableRoute(int slot) {
    const float z = .03f, px = (kPhoneX0 + kPhoneX1) / 2, ear = kHeadX + kBandR;
    switch (slot) {
        case SlotHeadset:  // hangs from the near ear cup
            return { { ear, kHeadY, kBandZ - .57f }, { ear + .04f, kHeadY, .62f }, { ear + .1f, kHeadY - .04f, .14f },
                     { ear + .22f, kHeadY - .26f, z }, { ear + .27f, .45f, z }, { ear + .22f, 0, z } };
        case SlotPhone:  // out of the bottom of the phone, around it
            return { { px, kPhoneY1 + .1f, z }, { px + .05f, kPhoneY1 + .3f, z }, { kPhoneX1 + .08f, kPhoneY1 + .34f, z },
                     { kPhoneX1 + .3f, kPhoneY1 + .04f, z }, { kPhoneX1 + .34f, 1.0f, z }, { kPhoneX1 + .28f, 0, z } };
        case SlotGamepad:  // from the top edge, around the headset stand
            return { { kPadCx, kPadY0 - .06f, z }, { kPadCx - .12f, 1.98f, z }, { .64f, 1.5f, z }, { .42f, .7f, z }, { .5f, 0, z } };
        case SlotKeyboard:  // behind the monitor
            return { { 3.4f, kKeysY0 - .08f, z }, { 3.32f, 1.65f, z }, { 3.5f, 1.0f, z }, { 3.42f, 0, z } };
        default:  // mouse: from between the buttons, past the monitor
            return { { kMouseX, kMouseY0 - .04f, z }, { kMouseX + .02f, 1.75f, z }, { kMouseX - .14f, 1.1f, z },
                     { kMouseX + .06f, .5f, z }, { kMouseX, 0, z } };
    }
}

// The part of a polyline between the fractions t0 and t1 of its length (`cum`: length up to each point).
std::vector<D2D1_POINT_2F> SubPath(const std::vector<D2D1_POINT_2F>& pts, const std::vector<float>& cum, float t0, float t1) {
    std::vector<D2D1_POINT_2F> out;
    float total = cum.back(), a = std::clamp(t0, 0.f, 1.f) * total, b = std::clamp(t1, 0.f, 1.f) * total;
    auto at = [&](float d) {
        size_t i = 1;
        while (i + 1 < pts.size() && cum[i] < d) i++;
        float span = cum[i] - cum[i - 1], k = span > 0 ? (d - cum[i - 1]) / span : 0;
        return D2D1_POINT_2F{ pts[i - 1].x + (pts[i].x - pts[i - 1].x) * k, pts[i - 1].y + (pts[i].y - pts[i - 1].y) * k };
    };
    if (b <= a) return out;
    out.push_back(at(a));
    for (size_t i = 1; i + 1 < pts.size(); i++)
        if (cum[i] > a && cum[i] < b) out.push_back(pts[i]);
    out.push_back(at(b));
    return out;
}

// A cable as drawn: the points along it from the plug to the back of the desk, the running length up to each
// point, and its shadow where it lies on the desk.
struct Cable {
    std::vector<D2D1_POINT_2F> pts, shadow;
    std::vector<float> cum;
};

Cable CablePath(const Iso& iso, int slot) {
    Cable cb;
    std::vector<P3> ctrl = CableRoute(slot);
    size_t n = ctrl.size();
    const int per = 8;
    for (size_t i = 0; i + 1 < n; i++) {  // Catmull-Rom through the route
        const P3 &p0 = ctrl[i ? i - 1 : 0], &p1 = ctrl[i], &p2 = ctrl[i + 1], &p3 = ctrl[std::min(i + 2, n - 1)];
        for (int k = 0; k < per + (i + 2 == n ? 1 : 0); k++) {
            float t = (float)k / per, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float cc, float d) {
                return .5f * (2 * b + (cc - a) * t + (2 * a - 5 * b + 4 * cc - d) * t2 + (3 * b - a - 3 * cc + d) * t3);
            };
            float x = cr(p0.x, p1.x, p2.x, p3.x), y = cr(p0.y, p1.y, p2.y, p3.y), z = std::max(.03f, cr(p0.z, p1.z, p2.z, p3.z));
            cb.pts.push_back(iso.P(x, y, z));
            if (z < .1f) cb.shadow.push_back(iso.P(x + .05f, y + .04f, 0));
        }
    }
    cb.cum.assign(cb.pts.size(), 0.f);
    for (size_t i = 1; i < cb.pts.size(); i++)
        cb.cum[i] = cb.cum[i - 1] + std::hypot(cb.pts[i].x - cb.pts[i - 1].x, cb.pts[i].y - cb.pts[i - 1].y);
    return cb;
}

// The cable itself and its plug: a short, thicker, lighter end at the device's port.
void DrawCable(const Iso& iso, const Materials& m, int slot) {
    Cable cb = CablePath(iso, slot);
    D2D1_COLOR_F shadow = m.shadow;
    shadow.a *= .7f;
    g_paint.Polyline(cb.shadow.data(), cb.shadow.size(), shadow, 1.6f);
    g_paint.Polyline(cb.pts.data(), cb.pts.size(), m.cable, 1.6f);
    D2D1_COLOR_F plug = Mix(m.cable, D2D1::ColorF(1, 1, 1, 1), g_model.light ? .1f : .35f);
    std::vector<D2D1_POINT_2F> end = SubPath(cb.pts, cb.cum, 0, 4.5f / cb.cum.back());
    g_paint.Polyline(end.data(), end.size(), plug, 3);
}

// The light on a cable: a steady glow at the plug and, while the device charges, a soft light that runs
// along the cable to the device. This is the only part of the desk that changes from frame to frame.
void DrawCableLight(const Iso& iso, const fluent::Palette& c, int slot, bool charging) {
    Cable cb = CablePath(iso, slot);
    D2D1_COLOR_F light = g_model.light ? c.accent : Mix(c.accent, D2D1::ColorF(1, 1, 1, 1), .25f), none = light;
    none.a = 0;
    float glow = .34f;
    if (charging && Animated()) {
        float t = std::fmod((float)(GetTickCount64() % 3600000) + slot * 420.f, kPulsePeriodMs);
        if (t < kPulseTravelMs) {
            float phase = t / kPulseTravelMs, pos = 1 - phase;
            float fade = std::sqrt(std::max(0.f, std::sin(kPi * phase)));  // in and out softly at the two ends
            const float halfLen[3] = { 13, 8, 3.5f }, alpha[3] = { .22f, .45f, .95f }, width[3] = { 3.6f, 2.4f, 1.6f };
            for (int k = 0; k < 3; k++) {
                float h = halfLen[k] / cb.cum.back();
                std::vector<D2D1_POINT_2F> part = SubPath(cb.pts, cb.cum, pos - h, pos + h);
                D2D1_COLOR_F col = light;
                col.a = alpha[k] * fade;
                g_paint.Polyline(part.data(), part.size(), col, width[k]);
            }
        }
        // The plug lights up as the light arrives.
        float arrive = std::fabs(t - kPulseTravelMs) / 380;
        if (arrive < 1) glow += .3f * (1 - arrive) * (1 - arrive);
    }
    light.a = glow;
    g_paint.RadialEllipse(cb.pts[0].x, cb.pts[0].y, 6.5f, 6.5f, light, none);
}

void DrawMonitor(const Iso& iso, const Materials& m, const fluent::Palette& c) {
    const float x0 = 2.3f, x1 = 7.7f, y0 = .4f, y1 = .52f, z0 = 1.15f, z1 = 3.3f, foot = .1f;
    // The stand: a base plate lying on the desk, and a neck that rises from the back of the plate to the
    // back of the panel. The panel hangs clear of the desk.
    Extrude(iso, RoundRect(kStandX0, kStandX1, kStandY0, kStandY1, .2f), 0, foot, m.body, &m.rim);
    g_paint.Transform(iso.Plane(foot));
    g_paint.Fill({ 4.74f, .16f, 5.3f, .5f }, .06f, Shade(m.body.top, .8f));  // the neck's shadow on the plate
    g_paint.ResetTransform();
    Extrude(iso, RoundRect(4.8f, 5.2f, .18f, .38f, .05f), foot, 2.2f, m.body);
    Extrude(iso, RoundRect(x0, x1, y0, y1, 0), z0, z1, m.body);
    g_paint.Line(iso.P(x0, y1, z1), iso.P(x1, y1, z1), m.rim, 1);
    // The display on the front face: a thin bezel, a deeper chin.
    const float in = .07f, chin = .13f;
    D2D1_POINT_2F s[4] = { iso.P(x0 + in, y1, z0 + chin), iso.P(x1 - in, y1, z0 + chin), iso.P(x1 - in, y1, z1 - in),
                           iso.P(x0 + in, y1, z1 - in) };
    Screen(s, 4, s[3], s[1], m, c.accent, g_model.light ? .46f : .40f);
    // A soft reflection band across the glass.
    auto on = [&](float u, float v) { return iso.P(x0 + in + (x1 - x0 - 2 * in) * u, y1, z0 + chin + (z1 - in - z0 - chin) * v); };
    D2D1_POINT_2F glare[4] = { on(.50f, 1), on(.72f, 1), on(.40f, 0), on(.18f, 0) };
    g_paint.Polygon(glare, 4, D2D1::ColorF(1, 1, 1, .045f));
}

void DrawHeadset(const Iso& iso, const Materials& m, const Shades& s) {
    const float cx = kHeadX, cy = kHeadY, R = kBandR, zc = kBandZ, half = .11f, thick = .08f;
    const float cupR = .37f, cupZ = zc - .2f;
    D2D1_COLOR_F cushion = Shade(s.top, g_model.light ? .42f : .36f), cushionDark = Shade(cushion, .7f);
    Extrude(iso, Oval(cx, cy, .5f, .5f), 0, .06f, m.body, &m.rim);  // stand
    // Far ear cup: we look into its cushion.
    CupX(iso, cx - R - .12f, cx - R + .04f, cy, cupZ, cupR, s.top, s.front, s.side);
    CupX(iso, cx - R + .04f, cx - R + .12f, cy, cupZ, cupR * .92f, cushion, cushionDark, cushion);
    CupX(iso, cx - R + .12f, cx - R + .12f, cy, cupZ, cupR * .5f, cushionDark, cushionDark, cushionDark);
    Extrude(iso, Oval(cx, cy, .075f, .075f, 12), .06f, zc + R - thick - .08f, m.body);
    Extrude(iso, RoundRect(cx - .2f, cx + .2f, cy - .14f, cy + .14f, .1f), zc + R - thick - .08f, zc + R - thick, m.body);
    // Headband: an arc in the x / z plane. Where the outside faces away we see the inside instead.
    auto at = [&](float deg, float r, float y) {
        float a = deg * kPi / 180;
        return iso.P(cx + r * std::cos(a), y, zc + r * std::sin(a));
    };
    const int n = 18;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            float a0 = 180 - 180.f * i / n, a1 = std::max(0.f, 180 - 180.f * (i + 1) / n - 1.5f);
            float am = (a0 + a1) / 2 * kPi / 180;
            if (pass == 0) {
                bool outer = std::cos(am) + .82f * std::sin(am) > 0;
                float r = outer ? R : R - thick;
                D2D1_COLOR_F col = outer ? Mix(s.side, s.top, std::sin(am)) : Shade(s.front, .8f);
                Quad(at(a0, r, cy - half), at(a1, r, cy - half), at(a1, r, cy + half), at(a0, r, cy + half), col);
            } else {
                Quad(at(a0, R, cy + half), at(a1, R, cy + half), at(a1, R - thick, cy + half), at(a0, R - thick, cy + half), s.front);
            }
        }
    // Near ear cup: cushion towards the head, then the shell with its cap.
    CupX(iso, cx + R - .12f, cx + R - .04f, cy, cupZ, cupR * .92f, cushion, cushionDark, cushion);
    CupX(iso, cx + R - .04f, cx + R + .12f, cy, cupZ, cupR, s.top, s.front, Mix(s.side, s.top, .55f));
    CupX(iso, cx + R + .12f, cx + R + .12f, cy, cupZ, cupR * .58f, s.side, s.side, s.side);
}

void DrawPhone(const Iso& iso, const Materials& m, const Shades& s, D2D1_COLOR_F glow, float strength) {
    const float z = .08f;
    Extrude(iso, RoundRect(kPhoneX0, kPhoneX1, kPhoneY0, kPhoneY1, .17f), 0, z, s, &m.rim);
    Outline glass = Project(iso, RoundRect(kPhoneX0 + .06f, kPhoneX1 - .06f, kPhoneY0 + .06f, kPhoneY1 - .06f, .12f), z);
    Screen(glass.data(), glass.size(), iso.P(kPhoneX0, kPhoneY0, z), iso.P(kPhoneX1, kPhoneY1, z), m, glow, strength);
    g_paint.Transform(iso.Plane(z));
    g_paint.Ellipse((kPhoneX0 + kPhoneX1) / 2, kPhoneY0 + .2f, .05f, .05f, Mix(m.screen, D2D1::ColorF(1, 1, 1, 1), .3f));  // camera
    g_paint.ResetTransform();
}

// The controller's outline seen from above: wide across the sticks, two grips towards the user.
Outline GamepadOutline() {
    static const float half[9][2] = { { .42f, 0 },    { .8f, .05f },  { .97f, .25f }, { 1, .56f },   { .95f, .85f },
                                      { .78f, 1 },    { .58f, .93f }, { .46f, .73f }, { .25f, .6f } };
    Outline ctrl;
    ctrl.push_back({ kPadCx, kPadY0 });
    for (int i = 0; i < 9; i++) ctrl.push_back({ kPadCx + half[i][0] * kPadHalf, kPadY0 + half[i][1] * kPadLen });
    ctrl.push_back({ kPadCx, kPadY0 + kPadNotch * kPadLen });
    for (int i = 8; i >= 0; i--) ctrl.push_back({ kPadCx - half[i][0] * kPadHalf, kPadY0 + half[i][1] * kPadLen });
    return Smooth(ctrl, 4);
}

void DrawGamepad(const Iso& iso, const Materials& m, const Shades& s) {
    const float z = .2f;
    Extrude(iso, GamepadOutline(), 0, z, s, &m.rim);
    D2D1_COLOR_F dark = Shade(s.top, .46f), mid = Shade(s.top, .7f);
    Shades stick = { mid, Shade(mid, .8f), Shade(mid, .66f) };
    auto ux = [](float u) { return kPadCx + u * kPadHalf; };
    auto vy = [](float v) { return kPadY0 + v * kPadLen; };
    const float lsx = ux(-.56f), lsy = vy(.27f), rsx = ux(.27f), rsy = vy(.41f);  // thumbsticks
    const float dx = ux(-.27f), dy = vy(.41f), bx = ux(.56f), by = vy(.27f);      // d-pad, face buttons
    g_paint.Transform(iso.Plane(z));
    g_paint.Ellipse(lsx, lsy, .17f, .17f, dark);
    g_paint.Ellipse(rsx, rsy, .17f, .17f, dark);
    g_paint.Fill({ dx - .15f, dy - .05f, dx + .15f, dy + .05f }, .02f, dark);
    g_paint.Fill({ dx - .05f, dy - .15f, dx + .05f, dy + .15f }, .02f, dark);
    for (int i = 0; i < 4; i++) {
        float a = i * kPi / 2;
        g_paint.Ellipse(bx + .125f * std::cos(a), by + .125f * std::sin(a), .058f, .058f, dark);
    }
    g_paint.Fill({ kPadCx - .13f, vy(.2f) - .035f, kPadCx - .03f, vy(.2f) + .035f }, .03f, dark);  // menu buttons
    g_paint.Fill({ kPadCx + .03f, vy(.2f) - .035f, kPadCx + .13f, vy(.2f) + .035f }, .03f, dark);
    g_paint.ResetTransform();
    Extrude(iso, Oval(lsx, lsy, .115f, .115f, 14), z, z + .08f, stick);
    Extrude(iso, Oval(rsx, rsy, .115f, .115f, 14), z, z + .08f, stick);
}

void DrawKeyboard(const Iso& iso, const Materials& m, const Shades& s) {
    const float z = .12f, pad = .08f, gap = .045f;
    // The case is a step darker than the key caps, so the gaps between the keys read as depth.
    Shades base = Solid(Shade(s.top, g_model.light ? .86f : .72f), m);
    Extrude(iso, RoundRect(kKeysX0, kKeysX1, kKeysY0, kKeysY1, .1f), 0, z, base, &m.rim);
    static const float rows[5][16] = {
        { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },
        { 1.5f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1.5f, 0 },
        { 1.75f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.25f, 0 },
        { 2.25f, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2.75f, 0 },
        { 1.25f, 1.25f, 1.25f, 6.25f, 1.25f, 1.25f, 1.25f, 1.25f, 0 },
    };
    float unit = (kKeysX1 - kKeysX0 - 2 * pad + gap) / 15, pitch = (kKeysY1 - kKeysY0 - 2 * pad + gap) / 5;
    g_paint.Transform(iso.Plane(z + .035f));
    for (int r = 0; r < 5; r++) {
        float x = kKeysX0 + pad, y = kKeysY0 + pad + r * pitch;
        for (int k = 0; rows[r][k] > 0; k++) {
            float w = rows[r][k] * unit;
            g_paint.Fill({ x, y, x + w - gap, y + pitch - gap }, .03f, s.top);
            x += w;
        }
    }
    g_paint.ResetTransform();
}

void DrawMouse(const Iso& iso, const Shades& s) {
    const float cx = kMouseX, cy = (kMouseY0 + kMouseY1) / 2, len = kMouseY1 - kMouseY0;
    const float wall = .1f, dome = .28f, hump = cy + len * .1f;  // the highest point sits towards the palm
    // Egg outline: the narrow end with the buttons points at the monitor, the wide palm end at the user.
    Outline egg;
    for (int i = 0; i < 32; i++) {
        float u = i * 2 * kPi / 32, sn = std::sin(u), cs = std::cos(u);
        float px = (sn < 0 ? -1.f : 1.f) * std::pow(std::fabs(sn), .85f), py = (cs < 0 ? -1.f : 1.f) * std::pow(std::fabs(cs), .85f);
        egg.push_back({ cx + kMouseW / 2 * px * (1 - .14f * cs), cy - len / 2 * py });
    }
    Walls(iso, egg, 0, wall, s);
    // The domed back: the outline shrinking towards the hump as it rises, lighter towards the top.
    const int layers = 9;
    D2D1_COLOR_F low = Mix(s.side, s.top, .45f);
    for (int k = 0; k <= layers; k++) {
        float a = k * (kPi / 2) * .86f / layers, sc = std::cos(a), z = wall + dome * std::sin(a);
        Outline o;
        for (const auto& p : egg) o.push_back({ cx + (p.x - cx) * sc, hump + (p.y - hump) * sc });
        Top(iso, o, z, Mix(low, s.top, (float)k / layers));
    }
    // A point on the shell above the centre line.
    auto surface = [&](float y) {
        float extent = y < hump ? hump - kMouseY0 : kMouseY1 - hump, rho = std::min(1.f, std::fabs(y - hump) / extent);
        return iso.P(cx, y, wall + dome * std::sqrt(1 - rho * rho));
    };
    D2D1_COLOR_F seam = Shade(s.top, .5f), wheel = Shade(s.top, .32f);
    float y0 = kMouseY0 + .05f, y1 = kMouseY0 + len * .46f;
    for (int i = 0; i < 6; i++)  // the split between the two buttons
        g_paint.Line(surface(y0 + (y1 - y0) * i / 6), surface(y0 + (y1 - y0) * (i + 1) / 6), seam, 1);
    D2D1_POINT_2F w0 = surface(kMouseY0 + len * .16f), w1 = surface(kMouseY0 + len * .34f);
    g_paint.Line({ w0.x, w0.y - .6f }, { w1.x, w1.y - .6f }, wheel, 3);
}

const int kDeskCables[] = { SlotGamepad, SlotKeyboard, SlotPhone, SlotMouse };  // these lie on the desk, under the objects

// The desk is drawn in two parts, so that the light on the cables can run between them: under the objects
// (the desk, its mat, the shadows, the cables on the desk), and the objects themselves.
void DrawDeskBase(const Iso& iso, const Materials& m, const fluent::Palette& c, const Placement& pl) {
    // The shadow the desk casts on the floor, then the slab itself.
    g_paint.Transform(iso.Plane(-.85f));
    g_paint.RadialEllipse(5.3f, 2.3f, 6.2f, 3.2f, m.ground, D2D1::ColorF(0, 0, 0, 0));
    g_paint.ResetTransform();
    Outline slab = RoundRect(0, 10, 0, 4, .16f);
    Walls(iso, slab, -.3f, 0, m.desk);
    {
        Outline p = Project(iso, slab, 0);
        g_paint.PolygonGradient(p.data(), p.size(), iso.P(0, 0, 0), iso.P(10, 4, 0), m.desk.top, Shade(m.desk.top, .9f));
        g_paint.Outline(p.data(), p.size(), m.rim, 1);
    }
    if (g_stage < 2) return;

    int hs = pl.device[SlotHeadset], ph = pl.device[SlotPhone], gp = pl.device[SlotGamepad], ms = pl.device[SlotMouse];

    // Desk mat under the keyboard and the mouse, and the monitor's light on the desk.
    g_paint.Transform(iso.Plane(0));
    g_paint.Fill({ 2.45f, 1.85f, 8.85f, 3.62f }, .14f, m.mat);
    D2D1_COLOR_F glow = c.accent, none = c.accent;
    glow.a = g_model.light ? .10f : .13f;
    none.a = 0;
    g_paint.RadialEllipse(5, 1.25f, 3.0f, .8f, glow, none);
    g_paint.ResetTransform();

    // All contact shadows first, so that no shadow lands on an object in front of it.
    Shadow(iso, kStandX0, kStandX1, kStandY0, kStandY1, .2f, m);
    if (hs >= 0) Shadow(iso, kHeadX - .5f, kHeadX + .5f, kHeadY - .5f, kHeadY + .5f, .5f, m);
    if (ph >= 0) Shadow(iso, kPhoneX0, kPhoneX1, kPhoneY0, kPhoneY1, .17f, m);
    if (gp >= 0) {
        Shadow(iso, kPadCx - kPadHalf + .06f, kPadCx + kPadHalf - .06f, kPadY0 + .05f, kPadY0 + kPadLen * .62f, .3f, m);
        Shadow(iso, kPadCx - kPadHalf + .06f, kPadCx - kPadHalf * .46f, kPadY0 + .5f, kPadY0 + kPadLen - .06f, .2f, m);
        Shadow(iso, kPadCx + kPadHalf * .46f, kPadCx + kPadHalf - .06f, kPadY0 + .5f, kPadY0 + kPadLen - .06f, .2f, m);
    }
    Shadow(iso, kKeysX0, kKeysX1, kKeysY0, kKeysY1, .1f, m);
    if (ms >= 0) Shadow(iso, kMouseX - kMouseW / 2 + .04f, kMouseX + kMouseW / 2 - .04f, kMouseY0 + .05f, kMouseY1 - .03f, .4f, m);

    for (int slot : kDeskCables)
        if (Powered(pl.device[slot])) DrawCable(iso, m, slot);
}

// The objects, from the back of the desk to the front.
void DrawDeskObjects(const Iso& iso, const Materials& m, const fluent::Palette& c, const Placement& pl, int hoverSlot) {
    if (g_stage < 2) return;
    int hs = pl.device[SlotHeadset], ph = pl.device[SlotPhone], gp = pl.device[SlotGamepad], kb = pl.device[SlotKeyboard],
        ms = pl.device[SlotMouse];
    DrawMonitor(iso, m, c);
    if (hs >= 0) {
        DrawHeadset(iso, m, Solid(DeviceTop(m, c, hs, hoverSlot == SlotHeadset), m));
        if (Powered(hs)) DrawCable(iso, m, SlotHeadset);  // hangs from the ear cup, in front of the stand
    }
    if (ph >= 0) {
        const auto& d = g_model.devices[ph];
        bool power = d.charging || d.full;
        DrawPhone(iso, m, Solid(DeviceTop(m, c, ph, hoverSlot == SlotPhone), m), d.low && !power ? c.critical : c.accent,
                  d.low && !power ? .7f : power ? .5f : .34f);
    }
    if (gp >= 0) DrawGamepad(iso, m, Solid(DeviceTop(m, c, gp, hoverSlot == SlotGamepad), m));
    // The keyboard is always on the desk; it has a battery level only when it is a wireless one.
    DrawKeyboard(iso, m, Solid(DeviceTop(m, c, kb, kb >= 0 && hoverSlot == SlotKeyboard), m));
    if (ms >= 0) DrawMouse(iso, Solid(DeviceTop(m, c, ms, hoverSlot == SlotMouse), m));
}

// The two parts of the desk are kept as layers and drawn again only when what they show changes (new
// readings, another hovered device, a rebuilt or resized target): a frame of the charging light then costs
// two bitmaps and a few strokes.
struct Scene {
    ID2D1Bitmap *base = nullptr, *objects = nullptr;
    unsigned paint = 0, model = 0;
    int hover = -2;
} g_scene;
unsigned g_modelStamp = 0;  // goes up with every new model

void DropScene() {
    if (g_scene.base) g_scene.base->Release();
    if (g_scene.objects) g_scene.objects->Release();
    g_scene.base = g_scene.objects = nullptr;
}

void DrawDesk(const Iso& iso, const Materials& m, const fluent::Palette& c, const Placement& pl, int hoverSlot) {
    if (!g_scene.base || !g_scene.objects || g_scene.paint != g_paint.Generation() || g_scene.model != g_modelStamp ||
        g_scene.hover != hoverSlot) {
        DropScene();
        if (g_paint.BeginLayer()) {
            DrawDeskBase(iso, m, c, pl);
            g_scene.base = g_paint.EndLayer();
        }
        if (g_paint.BeginLayer()) {
            DrawDeskObjects(iso, m, c, pl, hoverSlot);
            g_scene.objects = g_paint.EndLayer();
        }
        g_scene.paint = g_paint.Generation();
        g_scene.model = g_modelStamp;
        g_scene.hover = hoverSlot;
    }
    bool layers = g_scene.base && g_scene.objects;  // otherwise straight into the window
    if (layers) g_paint.DrawLayer(g_scene.base);
    else DrawDeskBase(iso, m, c, pl);
    if (g_stage >= 2)
        for (int slot : kDeskCables)
            if (Powered(pl.device[slot])) DrawCableLight(iso, c, slot, g_model.devices[pl.device[slot]].charging);
    if (layers) g_paint.DrawLayer(g_scene.objects);
    else DrawDeskObjects(iso, m, c, pl, hoverSlot);
    int hs = pl.device[SlotHeadset];
    if (g_stage >= 2 && Powered(hs)) DrawCableLight(iso, c, SlotHeadset, g_model.devices[hs].charging);
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
    // Centre the desk: its projection spans x from -3.46 to 8.66 units and y from about -2 (the top of the
    // monitor) to 7.3 units.
    Iso iso;
    iso.ox = kWidth / 2 - (8.66f - 3.46f) / 2 * kUnit;
    iso.oy = sceneTop + 2 * kUnit;

    Anchor anchors[SlotCount];
    int hoverSlot = -1;
    if (g_stage >= 1) {
        PlaceAnchors(iso, pl, anchors);
        for (int s = 0; s < SlotCount; s++) {
            if (!anchors[s].used || pl.device[s] < 0) continue;
            D2D1_RECT_F label = LabelRect(anchors[s], sceneTop, sceneBottom);
            if (Inside(anchors[s].bounds) || Inside(label)) hoverSlot = s;
            g_hits.push_back({ anchors[s].bounds, HitKind::Device, g_model.devices[pl.device[s]].key });
            g_hits.push_back({ label, HitKind::Device, g_model.devices[pl.device[s]].key });
        }
        DrawDesk(iso, m, c, pl, hoverSlot);
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
        SetTimer(g_wnd, kOpenTimer, 10, nullptr);
        g_timer = true;
    }
}

// The charging light needs redrawing only while the panel shows a charging device on the desk.
void SyncChargeTimer() {
    bool want = false;
    if (g_wnd && IsWindowVisible(g_wnd) && Animated()) {
        Placement p = Place();
        for (int slot = 0; slot < SlotCount; slot++)
            if (p.device[slot] >= 0 && g_model.devices[p.device[slot]].charging) want = true;
    }
    if (want && !g_chargeTimer) SetTimer(g_wnd, kChargeTimer, kChargeFrameMs, nullptr);
    if (!want && g_chargeTimer) KillTimer(g_wnd, kChargeTimer);
    g_chargeTimer = want;
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
            if (wp == kChargeTimer) {
                InvalidateRect(h, nullptr, FALSE);
                return 0;
            }
            if (Elapsed(g_openAt) < kRiseMs + 20) ApplyWindowPos();
            InvalidateRect(h, nullptr, FALSE);
            if (!Animating()) {
                KillTimer(h, kOpenTimer);
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
    g_modelStamp++;
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
    SyncChargeTimer();
    return true;
}

void Update(const Model& model) {
    if (!Visible()) return;
    bool theme = model.light != g_model.light || model.backdrop != g_model.backdrop;
    g_model = model;
    g_modelStamp++;
    if (theme) ApplyBackdrop();
    PlaceWindow();  // the list under the desk may have grown or shrunk
    SyncChargeTimer();
    InvalidateRect(g_wnd, nullptr, FALSE);
}

void Hide() {
    if (!g_wnd) return;
    if (g_timer) {
        KillTimer(g_wnd, kOpenTimer);
        g_timer = false;
    }
    if (g_chargeTimer) {
        KillTimer(g_wnd, kChargeTimer);
        g_chargeTimer = false;
    }
    ShowWindow(g_wnd, SW_HIDE);
    DropScene();
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }

bool JustClosed() { return g_closedAt && GetTickCount64() - g_closedAt < 250; }

}  // namespace flyout
