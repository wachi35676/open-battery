#include "widget_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

namespace {

using widget_icons::Sprite;
using widget_icons::State;

constexpr float kPi = 3.14159265f;

struct Col { float r, g, b, a; };
Col Hex(unsigned v, float a = 1) { return { ((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a }; }
Col Mix(Col a, Col b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t }; }
Col Shade(Col c, float k) { return { c.r * k, c.g * k, c.b * k, 1 }; }

// The three faces of a solid, as in the panel: the top is the lightest, the right side (x = x1) a step
// darker and the front (y = y1) the darkest.
struct Shades { Col top, side, front; };

struct Mat {
    Shades body;
    Col dark;    // screens, cushions, the gaps between keys
    Col rim;     // highlight along the top edges
    Col glow;    // the state's colour (charging blue, full green, low red); unused when normal
    Col cable;   // the charging cable
    float sideK, frontK;
    bool light;
    State state;
};

Shades Solid(Col top, const Mat& m) {
    top.a = 1;
    return { top, Shade(top, m.sideK), Shade(top, m.frontK) };
}

// The panel's desk colours would vanish on a taskbar (dark body on a dark bar, white body on a light
// one), so the strip has its own: a light body on a dark taskbar and a mid-grey one on a light taskbar.
// Faces are opaque, because the taskbar can be transparent over the wallpaper.
Mat Materials(State state, bool light) {
    Mat m{};
    m.light = light;
    m.state = state;
    Col top;
    if (light) {
        m.sideK = .74f, m.frontK = .56f;
        top = Hex(0x74747C);
        m.dark = Hex(0x1F1F23);
        m.rim = { 1, 1, 1, .34f };
        m.cable = Hex(0x5C5C64);
    } else {
        m.sideK = .72f, m.frontK = .52f;
        top = Hex(0xE6E6EA);
        m.dark = Hex(0x2B2B30);
        m.rim = { 1, 1, 1, .7f };
        m.cable = Hex(0x9C9CA6);
    }
    switch (state) {
        case State::Charging:
            m.glow = light ? Hex(0x0078D4) : Hex(0x60CDFF);
            top = Mix(top, m.glow, light ? .42f : .34f);
            break;
        case State::Full:
            m.glow = light ? Hex(0x107C10) : Hex(0x6CCB5F);
            top = Mix(top, m.glow, light ? .42f : .34f);
            break;
        case State::Low:
            m.glow = light ? Hex(0xC41E28) : Hex(0xFF5A5A);
            top = Mix(top, m.glow, .62f);
            break;
        default:
            m.glow = top;
            break;
    }
    m.body = Solid(top, m);
    return m;
}

bool Plugged(const Mat& m) { return m.state == State::Charging || m.state == State::Full; 
}

// ---- drawing list ------------------------------------------------------------------------------
// An icon is first recorded as a list of polygons in projected units, then scaled to the wanted pixel
// size and rasterised. That way every icon is fitted to its box whatever its proportions are.

struct Pt { float x, y; };
using Path = std::vector<Pt>;

struct Cmd {
    Path pts;
    Col col;
    bool snap = false;    // round every vertex to a whole pixel: crisp edges for boxy shapes
    float stroke = 0;     // > 0: a line of this width (pixels at 100%) instead of a filled polygon
    bool closed = false;  // stroke only
    bool atop = false;    // only where something opaque was drawn already (edge highlights)
    bool cable = false;   // the charging cable's centre line: also returned with the sprite, for the pulse
};

struct Scene {
    std::vector<Cmd> cmds;

    // 2:1 dimetric projection: the panel's view, with slopes that are exactly one pixel up for two across.
    static Pt P(float x, float y, float z) { return { x - y, (x + y) * .5f - z }; }

    void Fill(Path p, Col c, bool snap = false) {
        Cmd cmd;
        cmd.pts = std::move(p);
        cmd.col = c;
        cmd.snap = snap;
        cmds.push_back(std::move(cmd));
    }
    void Quad(Pt a, Pt b, Pt c, Pt d, Col col, bool snap = false) { Fill({ a, b, c, d }, col, snap); }
    void Stroke(Path p, Col c, float width, bool closed, bool atop, bool snap = false) {
        Cmd cmd;
        cmd.pts = std::move(p);
        cmd.col = c;
        cmd.snap = snap;
        cmd.stroke = width;
        cmd.closed = closed;
        cmd.atop = atop;
        cmds.push_back(std::move(cmd));
    }

    // A charging cable through the projected points `ctrl`, the first of which is the device's port: a
    // thick plain line, with the plug at the port in the state's colour (blue charging, green full).
    void Cable(const Path& ctrl, const Mat& m, float plugLen = .3f) {
        Path line;
        size_t n = ctrl.size();
        for (size_t i = 0; i + 1 < n; i++) {  // Catmull-Rom through the points
            const Pt &p0 = ctrl[i ? i - 1 : 0], &p1 = ctrl[i], &p2 = ctrl[i + 1], &p3 = ctrl[std::min(i + 2, n - 1)];
            for (int k = 0; k < 6; k++) {
                float t = k / 6.f, t2 = t * t, t3 = t2 * t;
                line.push_back({ .5f * (2 * p1.x + (p2.x - p0.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (3 * p1.x - p0.x - 3 * p2.x + p3.x) * t3),
                                 .5f * (2 * p1.y + (p2.y - p0.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (3 * p1.y - p0.y - 3 * p2.y + p3.y) * t3) });
            }
        }
        line.push_back(ctrl[n - 1]);
        Path plug = { line[0] };
        float len = 0;
        for (size_t i = 1; i < line.size() && len < plugLen; i++) {
            len += std::hypot(line[i].x - line[i - 1].x, line[i].y - line[i - 1].y);
            plug.push_back(line[i]);
        }
        Stroke(line, m.cable, 1.7f, false, false);
        cmds.back().cable = true;
        Stroke(plug, m.glow, 2.7f, false, false);
    }

    static Path Project(const Path& o, float z) {
        Path p;
        p.reserve(o.size());
        for (const Pt& q : o) p.push_back(P(q.x, q.y, z));
        return p;
    }
    void Top(const Path& o, float z, Col c, const Col* rim = nullptr, bool snap = false) {
        Path p = Project(o, z);
        Fill(p, c, snap);
        if (rim) Stroke(std::move(p), *rim, 1, true, true, snap);
    }
    // The walls of outline `o` (on the ground plane) from z0 to z1: every segment that faces the viewer,
    // shaded by where it faces, back to front.
    void Walls(const Path& o, float z0, float z1, const Shades& s, bool snap = false) {
        size_t n = o.size();
        float area = 0;
        for (size_t i = 0; i < n; i++) {
            const Pt &a = o[i], &b = o[(i + 1) % n];
            area += a.x * b.y - b.x * a.y;
        }
        float sign = area > 0 ? 1.f : -1.f;
        struct Wall { size_t i; float depth, w; };
        std::vector<Wall> walls;
        for (size_t i = 0; i < n; i++) {
            const Pt &a = o[i], &b = o[(i + 1) % n];
            float nx = (b.y - a.y) * sign, ny = -(b.x - a.x) * sign;  // outward normal
            if (nx + ny <= 0) continue;                                // faces away
            walls.push_back({ i, a.x + a.y + b.x + b.y, std::clamp(std::atan2(ny, nx) / (kPi / 2), 0.f, 1.f) });
        }
        std::sort(walls.begin(), walls.end(), [](const Wall& p, const Wall& q) { return p.depth < q.depth; });
        for (const Wall& wl : walls) {
            const Pt &a = o[wl.i], &b = o[(wl.i + 1) % n];
            Quad(P(a.x, a.y, z0), P(b.x, b.y, z0), P(b.x, b.y, z1), P(a.x, a.y, z1), Mix(s.side, s.front, wl.w), snap);
        }
    }
    void Extrude(const Path& o, float z0, float z1, const Shades& s, const Col* rim = nullptr, bool snap = false) {
        Walls(o, z0, z1, s, snap);
        Top(o, z1, s.top, rim, snap);
    }
};

Path Oval(float cx, float cy, float rx, float ry, int n = 28) {
    Path o;
    for (int i = 0; i < n; i++) {
        float a = i * 2 * kPi / n;
        o.push_back({ cx + rx * std::cos(a), cy + ry * std::sin(a) });
    }
    return o;
}

Path Rect(float x0, float x1, float y0, float y1) { return { { x1, y0 }, { x1, y1 }, { x0, y1 }, { x0, y0 } }; }

Path RoundRect(float x0, float x1, float y0, float y1, float r) {
    Path o;
    const float cx[4] = { x1 - r, x1 - r, x0 + r, x0 + r }, cy[4] = { y0 + r, y1 - r, y1 - r, y0 + r };
    const int n = 5;
    for (int k = 0; k < 4; k++)
        for (int i = 0; i <= n; i++) {
            float a = ((k - 1) * 90.f + i * 90.f / n) * kPi / 180;
            o.push_back({ cx[k] + r * std::cos(a), cy[k] + r * std::sin(a) });
        }
    return o;
}

// A closed smooth curve through `ctrl` (Catmull-Rom), `per` points for each span.
Path Smooth(const Path& ctrl, int per) {
    Path o;
    size_t n = ctrl.size();
    for (size_t i = 0; i < n; i++) {
        const Pt &p0 = ctrl[(i + n - 1) % n], &p1 = ctrl[i], &p2 = ctrl[(i + 1) % n], &p3 = ctrl[(i + 2) % n];
        for (int k = 0; k < per; k++) {
            float t = (float)k / per, t2 = t * t, t3 = t2 * t;
            o.push_back({ .5f * (2 * p1.x + (p2.x - p0.x) * t + (2 * p0.x - 5 * p1.x + 4 * p2.x - p3.x) * t2 + (3 * p1.x - p0.x - 3 * p2.x + p3.x) * t3),
                          .5f * (2 * p1.y + (p2.y - p0.y) * t + (2 * p0.y - 5 * p1.y + 4 * p2.y - p3.y) * t2 + (3 * p1.y - p0.y - 3 * p2.y + p3.y) * t3) });
        }
    }
    return o;
}

// ---- the devices -------------------------------------------------------------------------------

// A slim mouse with its buttons towards the viewer, so the split between them and the wheel face us; the
// back rises to a low hump and widens only a little at the palm.
void Mouse(Scene& sc, const Mat& m) {
    const float W = .92f, len = 1.72f, wall = .15f, dome = .4f, hump = -len * .14f;
    const Shades& s = m.body;
    Path egg;
    for (int i = 0; i < 32; i++) {
        float u = i * 2 * kPi / 32, sn = std::sin(u), cs = std::cos(u);
        float px = (sn < 0 ? -1.f : 1.f) * std::pow(std::fabs(sn), .8f), py = (cs < 0 ? -1.f : 1.f) * std::pow(std::fabs(cs), .8f);
        egg.push_back({ W / 2 * px * (1 + .06f * cs), -len / 2 * py });
    }
    sc.Walls(egg, 0, wall, s);
    const int layers = 8;
    Col low = Mix(s.side, s.top, .55f);
    for (int k = 0; k <= layers; k++) {
        float a = k * (kPi / 2) * .88f / layers, c = std::cos(a), z = wall + dome * std::sin(a);
        Path o;
        for (const Pt& p : egg) o.push_back({ p.x * c, hump + (p.y - hump) * c });
        sc.Top(o, z, Mix(low, s.top, (float)k / layers), k == 0 ? &m.rim : nullptr);
    }
    // A point on the shell.
    auto surface = [&](float x, float y) {
        float extent = y < hump ? hump + len / 2 : len / 2 - hump;
        float u = x / (W / 2), v = (y - hump) / extent, rho2 = std::min(1.f, u * u + v * v);
        return Scene::P(x, y, wall + dome * std::sqrt(1 - rho2));
    };
    const float tip = len / 2 - .05f, mid = len * .04f;
    Path seam, split;
    for (int i = 0; i <= 6; i++) seam.push_back(surface(0, tip + (mid - tip) * i / 6));
    for (int i = 0; i <= 8; i++) split.push_back(surface(W * .43f * (i - 4) / 4, mid + .1f * std::fabs(i - 4.f) / 4));
    Col line = Mix(s.top, m.dark, .7f);
    sc.Stroke(split, line, 1, false, true);  // where the buttons end
    sc.Stroke(seam, line, 1, false, true);   // the split between the two buttons
    sc.Stroke({ surface(0, len / 2 - len * .3f), surface(0, len / 2 - len * .15f) }, m.dark, 2.2f, false, true);  // wheel
    if (Plugged(m))  // the cable leaves at the front, between the buttons
        sc.Cable({ Scene::P(0, len / 2 - .03f, .05f), Scene::P(0, len / 2 + .32f, 0), Scene::P(-.3f, len / 2 + .58f, 0), Scene::P(-.8f, len / 2 + .52f, 0) }, m);
}

// A slab with three rows of chunky key caps; the case is darker than the caps so the gaps read as depth.
void Keyboard(Scene& sc, const Mat& m) {
    const float X = 2.3f, Y = 1.0f, z = .2f, pad = .1f, gap = .09f;
    Shades base = Solid(Shade(m.body.top, m.light ? .5f : .46f), m);
    if (Plugged(m))  // behind the back edge
        sc.Cable({ Scene::P(X * .7f, .04f, z * .6f), Scene::P(X * .7f, -.3f, 0), Scene::P(X * .7f + .25f, -.52f, 0), Scene::P(X * .7f + .75f, -.5f, 0) }, m);
    sc.Walls(Rect(0, X, 0, Y), 0, z, base, true);
    sc.Top(Rect(0, X, 0, Y), z, base.top, nullptr, true);
    Col caps = m.light ? Mix(m.body.top, Hex(0xFFFFFF), .3f) : m.body.top;
    static const float rows[3][6] = { { 1, 1, 1, 1, 1, 0 }, { 1, 1, 1, 1, 1, 0 }, { 1, 3, 1, 0 } };
    float unit = (X - 2 * pad + gap) / 5, pitch = (Y - 2 * pad + gap) / 3;
    for (int r = 0; r < 3; r++) {
        float x = pad, y = pad + r * pitch;
        for (int k = 0; rows[r][k] > 0; k++) {
            float w = rows[r][k] * unit;
            sc.Top(Rect(x, x + w - gap, y, y + pitch - gap), z + .03f, caps);
            x += w;
        }
    }
}

// The phone stands upright, its screen towards the viewer's right: lying flat it would be a sliver that
// reads as a keyboard at this size. The screen lights up in the state's colour.
void Phone(Scene& sc, const Mat& m) {
    const float t = .2f, Wd = 1.0f, Hh = 1.9f, r = .16f, in = .1f;
    const Shades& s = m.body;
    auto face = [&](float x, float inset, float rr) {  // rounded rectangle in the y / z plane
        Path o = RoundRect(inset, Wd - inset, inset, Hh - inset, rr), p;
        for (const Pt& q : o) p.push_back(Scene::P(x, q.x, q.y));
        return p;
    };
    for (int k = 0; k <= 4; k++) sc.Fill(face(t * k / 4, 0, r), s.front);                                          // thickness
    sc.Quad(Scene::P(0, r, Hh), Scene::P(t, r, Hh), Scene::P(t, Wd - r, Hh), Scene::P(0, Wd - r, Hh), s.top);     // top edge
    Path front = face(t, 0, r);
    sc.Fill(front, Mix(s.side, s.top, .6f));
    sc.Stroke(front, m.rim, 1, true, true);
    Col screen = m.state == State::Normal ? Mix(m.dark, s.top, m.light ? 0.f : .08f) : Mix(m.dark, m.glow, m.light ? .75f : .6f);
    sc.Fill(face(t, in, r - in * .6f), screen);
    if (m.state == State::Normal)  // a glint across the glass, so an unlit screen is not a hole in a dark taskbar
        sc.Quad(Scene::P(t, Wd - in, Hh - in - .1f), Scene::P(t, Wd - in, Hh * .62f), Scene::P(t, in, Hh * .8f), Scene::P(t, in + .1f, Hh - in),
                Mix(screen, s.top, m.light ? .2f : .16f));
    if (Plugged(m))  // from the port in the bottom edge
        sc.Cable({ Scene::P(t, Wd / 2, .02f), Scene::P(t + .32f, Wd / 2, 0), Scene::P(t + .52f, Wd / 2 + .22f, 0), Scene::P(t + .5f, Wd / 2 + .62f, 0) }, m);
}

// A controller with real volume: thick walls, a domed top, raised thumbsticks that cast a small shadow and
// buttons that sit on the shell. The outline is the panel's: wide across the sticks, two grips towards
// the viewer.
void Gamepad(Scene& sc, const Mat& m) {
    const float half = 1.f, len = 1.34f, notch = .6f, wall = .4f, dome = .16f, zt = wall + dome;
    const Shades& s = m.body;
    static const float h[9][2] = { { .42f, 0 },  { .8f, .05f },  { .97f, .25f }, { 1, .56f },  { .95f, .85f },
                                   { .78f, 1 },  { .58f, .93f }, { .44f, .75f }, { .25f, .62f } };
    Path ctrl;
    ctrl.push_back({ 0, 0 });
    for (int i = 0; i < 9; i++) ctrl.push_back({ h[i][0] * half, h[i][1] * len });
    ctrl.push_back({ 0, notch * len });
    for (int i = 8; i >= 0; i--) ctrl.push_back({ -h[i][0] * half, h[i][1] * len });
    Path o = Smooth(ctrl, 4);
    if (Plugged(m))  // from the top edge, between the shoulders
        sc.Cable({ Scene::P(0, .03f, wall - .04f), Scene::P(0, -.3f, wall - .2f), Scene::P(.3f, -.58f, .08f), Scene::P(.85f, -.58f, 0) }, m);
    sc.Walls(o, 0, wall, s);
    const int layers = 4;
    Col low = Mix(s.side, s.top, .6f);
    for (int k = 0; k <= layers; k++) {  // the top bulges: the outline shrinks a little as it rises
        float t = (float)k / layers, c = 1 - .1f * t * t;
        Path q;
        for (const Pt& p : o) q.push_back({ p.x * c, len * .45f + (p.y - len * .45f) * c });
        sc.Top(q, wall + dome * std::sin(t * kPi / 2), Mix(low, s.top, t), k == 0 ? &m.rim : nullptr);
    }
    Col dark = Mix(s.top, m.dark, .8f), shadow = Mix(s.top, m.dark, .38f);
    Shades cap = { Mix(s.top, m.dark, .62f), Mix(s.top, m.dark, .8f), Mix(s.top, m.dark, .92f) };
    const float lsx = -.52f * half, lsy = .27f * len, rsx = .25f * half, rsy = .44f * len;  // thumbsticks
    const float dx = -.25f * half, dy = .44f * len, bx = .52f * half, by = .27f * len;      // d-pad, face buttons
    sc.Top(Rect(dx - .18f, dx + .18f, dy - .065f, dy + .065f), zt, dark);
    sc.Top(Rect(dx - .065f, dx + .065f, dy - .18f, dy + .18f), zt, dark);
    for (int i = 0; i < 2; i++) {
        float x = i ? rsx : lsx, y = i ? rsy : lsy;
        sc.Top(Oval(x + .05f, y + .07f, .18f, .18f, 16), zt, shadow);
        sc.Extrude(Oval(x, y, .17f, .17f, 16), zt, zt + .17f, cap);
    }
    for (int i = 0; i < 2; i++)  // two face buttons, proud of the shell
        sc.Extrude(Oval(bx + (i ? .13f : -.11f), by + (i ? -.07f : .11f), .105f, .105f, 12), zt, zt + .07f, cap);
}

// Headphones as the familiar silhouette: a thick arch over two chunky ear cups, seen from the front and
// a little from the side. A true isometric model of something this thin reads as a telephone handset at
// 24 pixels, so this one is drawn face-on and given depth along the isometric diagonal (back = up-right).
void Headset(Scene& sc, const Mat& m) {
    const float R = 1.f, r = .7f, c = .8f;                  // band radii, height of the band's centre
    const float cx = (R + r) / 2, cw = .68f, cTop = c + .24f;  // ear cups: centre, width, top (they stand on v = 0)
    const float depth = .34f;
    const Shades& s = m.body;
    auto at = [&](float u, float v, float d) { return Pt{ u + d * depth, -v - d * depth / 2 }; };
    auto arch = [&](float d) {
        Path p;
        for (int i = 0; i <= 20; i++) p.push_back(at(R * std::cos(i * kPi / 20), c + R * std::sin(i * kPi / 20), d));
        for (int i = 20; i >= 0; i--) p.push_back(at(r * std::cos(i * kPi / 20), c + r * std::sin(i * kPi / 20), d));
        return p;
    };
    auto pad = [&](float u0, float u1, float v0, float v1, float rr, float d) {
        Path o = RoundRect(u0, u1, v0, v1, rr), p;
        for (const Pt& q : o) p.push_back(at(q.x, q.y, d));
        return p;
    };
    auto cup = [&](int side, float d) { return pad(side * cx - cw / 2, side * cx + cw / 2, 0, cTop, .27f, d); };
    const int steps = 6;
    for (int k = steps; k >= 0; k--) {  // the depth, back to front: lit from above
        float d = (float)k / steps;
        sc.Fill(cup(-1, d), Mix(s.top, s.side, .35f));
        sc.Fill(cup(1, d), Mix(s.top, s.side, .35f));
        sc.Fill(arch(d), s.top);
    }
    // What shows of that depth between the cups is the inside of the band and of the left cup: in shade.
    Path inside = { at(r, 0, 0) };
    for (int i = 0; i <= 20; i++) inside.push_back(at(r * std::cos(i * kPi / 20), c + r * std::sin(i * kPi / 20), 0));
    inside.push_back(at(-r, 0, 0));
    Cmd shade;
    shade.pts = inside;
    shade.col = Shade(s.front, .8f);
    shade.atop = true;
    sc.cmds.push_back(shade);
    // The faces towards us.
    sc.Fill(arch(0), Mix(s.side, s.top, .3f));
    Col cushion = Mix(s.top, m.dark, m.light ? .72f : .8f);
    for (int side = -1; side <= 1; side += 2) {
        Path face = cup(side, 0);
        sc.Fill(face, Mix(s.side, s.top, .55f));
        sc.Stroke(face, m.rim, 1, true, true);
        float in = side * (cx - cw / 2);  // the cushion, on the side of the head
        sc.Fill(pad(std::min(in, in + side * .24f), std::max(in, in + side * .24f), .12f, cTop - .12f, .11f, 0), cushion);
    }
    if (Plugged(m))  // from under the left ear cup
        sc.Cable({ at(-cx - .05f, .03f, 0), at(-cx - .05f, -.22f, 0), at(-cx - .3f, -.42f, 0), at(-cx - .75f, -.4f, 0) }, m);
}

// Desk microphone: round base, short stem, a tall capsule whose grille is marked by two dark bands.
void Microphone(Scene& sc, const Mat& m) {
    const float r = .36f, z0 = .5f, z1 = 1.5f;
    const Shades& s = m.body;
    sc.Extrude(Oval(0, 0, .56f, .56f), 0, .12f, s, &m.rim);
    sc.Extrude(Oval(0, 0, .11f, .11f, 12), .12f, z0, Solid(Shade(s.top, .8f), m));
    sc.Top(Oval(0, 0, r, r), z0, s.front);  // underside of the capsule, so the stem meets something
    sc.Walls(Oval(0, 0, r, r), z0, z1, s);
    Shades band = { m.dark, Mix(s.side, m.dark, .85f), Mix(s.front, m.dark, .85f) };
    sc.Walls(Oval(0, 0, r, r), z0 + .3f, z0 + .44f, band);
    sc.Walls(Oval(0, 0, r, r), z0 + .62f, z0 + .76f, band);
    const int layers = 7;
    Col low = Mix(s.side, s.top, .5f);
    for (int k = 0; k <= layers; k++) {
        float a = k * (kPi / 2) * .9f / layers, c = std::cos(a);
        sc.Top(Oval(0, 0, r * c, r * c), z1 + r * .95f * std::sin(a), Mix(low, s.top, (float)k / layers), k == 0 ? &m.rim : nullptr);
    }
    if (Plugged(m))
        sc.Cable({ Scene::P(0, .54f, .06f), Scene::P(0, .86f, 0), Scene::P(-.26f, 1.1f, 0), Scene::P(-.74f, 1.08f, 0) }, m);
}

// Anything else that reports a battery: an upright cell with its cap.
void Cell(Scene& sc, const Mat& m) {
    const float r = .5f, h = 1.5f;
    const Shades& s = m.body;
    sc.Walls(Oval(0, 0, r, r), 0, h, s);
    Shades band = { m.dark, Mix(s.side, m.dark, .85f), Mix(s.front, m.dark, .85f) };
    sc.Walls(Oval(0, 0, r, r), h - .5f, h - .3f, band);
    sc.Top(Oval(0, 0, r, r), h, s.top, &m.rim);
    sc.Extrude(Oval(0, 0, .22f, .22f, 16), h, h + .2f, Solid(Shade(s.top, .82f), m));
    if (Plugged(m))
        sc.Cable({ Scene::P(0, r - .02f, .08f), Scene::P(0, r + .3f, 0), Scene::P(-.26f, r + .54f, 0), Scene::P(-.74f, r + .52f, 0) }, m);
}

// ---- rasteriser --------------------------------------------------------------------------------
// Every pixel has S x S samples that hold a colour, and a polygon overwrites the samples it covers. So
// two faces that share an edge never let the background through, which blending coverage masks would.

struct Raster {
    static constexpr int S = 6;
    int w, h;
    std::vector<float> buf;  // premultiplied r, g, b, a for every sample
    std::vector<unsigned char> mark;
    Raster(int w_, int h_) : w(w_), h(h_), buf((size_t)w_ * S * h_ * S * 4, 0.f), mark((size_t)w_ * S * h_ * S, 0) {}

    void Mark(const Path& p) {  // even-odd scan conversion at the sample centres
        size_t n = p.size();
        std::vector<float> xs;
        for (int j = 0; j < h * S; j++) {
            float y = (j + .5f) / S;
            xs.clear();
            for (size_t i = 0; i < n; i++) {
                const Pt &a = p[i], &b = p[(i + 1) % n];
                if ((a.y <= y) == (b.y <= y)) continue;
                xs.push_back(a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                int i0 = std::max(0, (int)std::ceil(xs[k] * S - .5f)), i1 = std::min(w * S - 1, (int)std::ceil(xs[k + 1] * S - .5f) - 1);
                for (int i = i0; i <= i1; i++) mark[(size_t)j * w * S + i] = 1;
            }
        }
    }
    void Blit(const std::vector<DWORD>& px, int n, int x0, int y0) {  // an n x n premultiplied BGRA picture
        for (int y = 0; y < n * S; y++)
            for (int x = 0; x < n * S; x++) {
                DWORD v = px[(size_t)(y / S) * n + x / S];
                float* d = &buf[((size_t)(y0 * S + y) * w * S + x0 * S + x) * 4];
                d[0] = ((v >> 16) & 255) / 255.f, d[1] = ((v >> 8) & 255) / 255.f, d[2] = (v & 255) / 255.f, d[3] = (v >> 24) / 255.f;
            }
    }
    void Flush(Col c, bool atop) {
        for (size_t i = 0; i < mark.size(); i++) {
            if (!mark[i]) continue;
            mark[i] = 0;
            float* d = &buf[i * 4];
            if (atop && d[3] < .99f) continue;
            float inv = 1 - c.a;
            d[0] = c.r * c.a + d[0] * inv;
            d[1] = c.g * c.a + d[1] * inv;
            d[2] = c.b * c.a + d[2] * inv;
            d[3] = c.a + d[3] * inv;
        }
    }
    Sprite Resolve(int& cropX, int& cropY) const {  // average the samples and crop to the ink
        std::vector<DWORD> px((size_t)w * h, 0);
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float acc[4] = {};
                for (int sy = 0; sy < S; sy++)
                    for (int sx = 0; sx < S; sx++) {
                        const float* d = &buf[((size_t)(y * S + sy) * w * S + x * S + sx) * 4];
                        for (int k = 0; k < 4; k++) acc[k] += d[k];
                    }
                auto byte = [&](float v) { return (DWORD)std::clamp((int)std::lround(v * 255 / (S * S)), 0, 255); };
                DWORD a = byte(acc[3]);
                if (!a) continue;
                px[(size_t)y * w + x] = (a << 24) | (std::min(a, byte(acc[0])) << 16) | (std::min(a, byte(acc[1])) << 8) | std::min(a, byte(acc[2]));
                x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
        Sprite s;
        if (x1 < 0) return s;
        cropX = x0, cropY = y0;
        s.w = x1 - x0 + 1;
        s.h = y1 - y0 + 1;
        s.px.resize((size_t)s.w * s.h);
        for (int y = 0; y < s.h; y++)
            for (int x = 0; x < s.w; x++) s.px[(size_t)y * s.w + x] = px[(size_t)(y + y0) * w + x + x0];
        return s;
    }
};

struct Design {
    void (*draw)(Scene&, const Mat&);
    float maxW, maxH;  // the box the icon is fitted into, as fractions of the icon size
};

Design DesignOf(DeviceKind kind) {
    switch (kind) {
        case DeviceKind::Headset:    return { Headset, 1.12f, 1.f };
        case DeviceKind::Mouse:      return { Mouse, .98f, .84f };
        case DeviceKind::Keyboard:   return { Keyboard, 1.36f, 1.f };
        case DeviceKind::Gamepad:    return { Gamepad, 1.3f, 1.f };
        case DeviceKind::Phone:      return { Phone, 1.f, .96f };
        case DeviceKind::Microphone: return { Microphone, 1.f, 1.f };
        default:                     return { Cell, 1.f, .94f };
    }
}

// Rasterises the drawing list scaled by `s` and moved by (ox, oy); `line` is the width of a 1-pixel
// stroke. Returns the cable's centre line in pixels, from the port outwards (empty without a cable).
Path Rasterise(Raster& rs, const std::vector<Cmd>& cmds, float s, int ox, int oy, float line) {
    Path cable;
    for (const Cmd& c : cmds) {
        Path p;
        p.reserve(c.pts.size());
        for (const Pt& q : c.pts) {
            float x = q.x * s, y = q.y * s;
            if (c.snap) x = std::round(x), y = std::round(y);
            p.push_back({ x + ox, y + oy });
        }
        if (c.cable) cable = p;
        if (c.stroke <= 0) {
            rs.Mark(p);
        } else {
            float hw = c.stroke * line / 2;
            size_t n = p.size(), segs = c.closed ? n : n - 1;
            for (size_t i = 0; i < segs; i++) {
                Pt a = p[i], b = p[(i + 1) % n];
                float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
                if (len < 1e-4f) continue;
                float nx = -dy / len * hw, ny = dx / len * hw;
                rs.Mark({ { a.x + nx, a.y + ny }, { b.x + nx, b.y + ny }, { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny } });
                if (c.stroke > 1.5f && !c.closed) {  // round joints and ends on the thick lines (cables)
                    Path dot;
                    for (int k = 0; k < 10; k++) dot.push_back({ b.x + hw * std::cos(k * kPi / 5), b.y + hw * std::sin(k * kPi / 5) });
                    rs.Mark(dot);
                    if (i == 0) {
                        for (Pt& q : dot) q.x += a.x - b.x, q.y += a.y - b.y;
                        rs.Mark(dot);
                    }
                }
            }
        }
        rs.Flush(c.col, c.atop);
    }
    return cable;
}

// One of the drawn icons above: the fallback, and the battery cell for devices of no known kind.
Sprite Render(DeviceKind kind, State state, bool light, int size) {
    Design d = DesignOf(kind);
    Scene sc;
    d.draw(sc, Materials(state, light));
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (const Cmd& c : sc.cmds) {
        if (c.stroke > 0) continue;
        for (const Pt& p : c.pts) {
            minX = std::min(minX, p.x), maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y), maxY = std::max(maxY, p.y);
        }
    }
    if (maxX <= minX || maxY <= minY) return {};
    float s = std::min(size * d.maxW / (maxX - minX), size * d.maxH / (maxY - minY));
    // The device alone decides the scale, so that it does not shrink when a cable is plugged in; the
    // bitmap then grows to hold the cable.
    float allX0 = minX, allX1 = maxX, allY0 = minY, allY1 = maxY;
    for (const Cmd& c : sc.cmds)
        for (const Pt& p : c.pts) {
            allX0 = std::min(allX0, p.x), allX1 = std::max(allX1, p.x);
            allY0 = std::min(allY0, p.y), allY1 = std::max(allY1, p.y);
        }
    float line = std::max(1.f, std::round(size / 24.f));  // one pixel at 100% and 125%, two at 200%
    const int pad = 2 + (int)std::ceil(2 * line);
    int ox = pad - (int)std::floor(allX0 * s), oy = pad - (int)std::floor(allY0 * s);
    Raster rs((int)std::ceil((allX1 - allX0) * s) + 2 * pad + 1, (int)std::ceil((allY1 - allY0) * s) + 2 * pad + 1);
    Path cable = Rasterise(rs, sc.cmds, s, ox, oy, line);
    int cropX = 0, cropY = 0;
    Sprite out = rs.Resolve(cropX, cropY);
    out.bodyY = (int)std::lround(minY * s) + oy - cropY;
    out.bodyH = (int)std::lround((maxY - minY) * s);
    for (auto it = cable.rbegin(); it != cable.rend(); ++it) out.cable.push_back({ it->x - cropX, it->y - cropY });  // towards the device
    return out;
}

// ---- Windows' own device icons -----------------------------------------------------------------
// The strip shows the icons Windows itself uses for devices (Devices and Printers), read at run time from
// DDORes.dll in System32; nothing of them is in this program. They come hand-drawn at 16, 20, 24, 28, 32
// and 48 pixels, so one is loaded at exactly the size it is shown at. Where the file or an icon is missing
// (another Windows version), the drawn icons above are used instead.

struct SystemIcon {
    int id;       // icon group resource in DDORes.dll
    float port;   // where the charging cable goes in: across the icon's width, 0 = left edge, 1 = right edge
    int dir;      // ... at its bottom edge (+1) or its top edge (-1)
    int side;     // the cable then turns left (-1) or right (+1)
};

bool SystemIconOf(DeviceKind kind, SystemIcon& out) {
    switch (kind) {
        case DeviceKind::Headset:    out = { 2013, .14f, 1, -1 }; return true;   // under the left ear cup
        case DeviceKind::Microphone: out = { 2014, .5f, 1, -1 }; return true;
        case DeviceKind::Phone:      out = { 2035, .5f, 1, -1 }; return true;
        case DeviceKind::Gamepad:    out = { 2207, .5f, -1, 1 }; return true;   // between the shoulders
        case DeviceKind::Keyboard:   out = { 2210, .74f, -1, 1 }; return true;
        case DeviceKind::Mouse:      out = { 2212, .5f, -1, 1 }; return true;   // at the front, between the buttons
        default: return false;  // no fitting system icon: the drawn battery cell
    }
}

// The icon as n x n premultiplied BGRA pixels; empty when it cannot be had.
std::vector<DWORD> LoadSystemIcon(int id, int n) {
    static HMODULE mod = LoadLibraryExW(L"DDORes.dll", nullptr,
                                        LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::vector<DWORD> px;
    HICON icon = mod ? (HICON)LoadImageW(mod, MAKEINTRESOURCEW(id), IMAGE_ICON, n, n, LR_DEFAULTCOLOR) : nullptr;
    if (!icon) return px;
    ICONINFO info{};
    if (GetIconInfo(icon, &info)) {
        if (info.hbmColor) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = n;
            bi.bmiHeader.biHeight = -n;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            px.resize((size_t)n * n);
            HDC dc = GetDC(nullptr);
            bool ok = GetDIBits(dc, info.hbmColor, 0, n, px.data(), &bi, DIB_RGB_COLORS) == n;
            ReleaseDC(nullptr, dc);
            bool alpha = false;
            for (DWORD& v : px) {  // straight alpha to premultiplied
                DWORD a = v >> 24;
                alpha = alpha || a;
                v = (a << 24) | ((((v >> 16) & 255) * a / 255) << 16) | ((((v >> 8) & 255) * a / 255) << 8) | ((v & 255) * a / 255);
            }
            if (!ok || !alpha) px.clear();  // an icon without an alpha channel is not one of the ones we expect
        }
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
    }
    DestroyIcon(icon);
    return px;
}

// The system icon for `kind`, with a cable plugged in while it is on power: a neutral lead with the plug
// in blue (charging) or green (full). The icon itself is never tinted.
Sprite RenderSystem(DeviceKind kind, State state, bool light, int size) {
    SystemIcon si{};
    if (!SystemIconOf(kind, si)) return {};
    int n = size;
    if (size < 56) {  // the nearest size the icon was drawn at; above those it is scaled from its 256 px picture
        static const int drawn[] = { 16, 20, 24, 28, 32, 48 };
        n = drawn[0];
        for (int d : drawn)
            if (std::abs(d - size) < std::abs(n - size)) n = d;
    }
    std::vector<DWORD> px = LoadSystemIcon(si.id, n);
    if (px.empty()) return {};
    auto alpha = [&](int x, int y) { return (int)(px[(size_t)y * n + x] >> 24); };
    int x0 = n, y0 = n, x1 = -1, y1 = -1;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (alpha(x, y) > 24) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    if (x1 < 0) return {};
    float k = n / 24.f, line = std::max(1.f, std::round(k));
    int margin = (int)std::ceil(11 * k) + 2;  // room for the cable around the icon
    Raster rs(n + 2 * margin, n + 2 * margin);
    rs.Blit(px, n, margin, margin);
    Path cable;
    if (state == State::Charging || state == State::Full) {
        // The port: the icon's outermost solid pixel in the port's column.
        int col = x0 + (int)std::lround(si.port * (x1 - x0)), row = si.dir > 0 ? y1 : y0;
        for (int y = si.dir > 0 ? y1 : y0; y >= y0 && y <= y1; y -= si.dir)
            if (alpha(col, y) > 128) { row = y; break; }
        float px0 = margin + col + .5f, py0 = margin + row + .5f, d = (float)si.dir, sd = (float)si.side;
        Scene sc;
        sc.Cable({ { px0, py0 }, { px0, py0 + d * 3.6f * k }, { px0 + sd * 2.6f * k, py0 + d * 6.2f * k },
                   { px0 + sd * 7.5f * k, py0 + d * 6.4f * k } },
                 Materials(state, light), 3 * k);
        cable = Rasterise(rs, sc.cmds, 1, 0, 0, line);
    }
    int cropX = 0, cropY = 0;
    Sprite out = rs.Resolve(cropX, cropY);
    out.bodyY = margin + y0 - cropY;
    out.bodyH = y1 - y0 + 1;
    for (auto it = cable.rbegin(); it != cable.rend(); ++it) out.cable.push_back({ it->x - cropX, it->y - cropY });  // towards the device
    return out;
}

}  // namespace

namespace widget_icons {

const Sprite& Get(DeviceKind kind, State state, bool light, int size) {
    static std::map<std::tuple<int, int, bool, int>, Sprite> cache;
    size = std::clamp(size, 8, 96);
    auto key = std::make_tuple((int)kind, (int)state, light, size);
    auto it = cache.find(key);
    if (it == cache.end()) {
        Sprite s = RenderSystem(kind, state, light, size);
        if (!s.w) s = Render(kind, state, light, size);
        it = cache.emplace(key, std::move(s)).first;
    }
    return it->second;
}

}  // namespace widget_icons
