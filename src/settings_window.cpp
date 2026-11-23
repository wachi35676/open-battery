#include "settings_window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "fluent.h"
#include "i18n.h"

namespace settings_window {
namespace {

using fluent::Look;

// Layout in DIPs, measured from the Windows 11 Settings app at 100% scale: a 280 wide navigation list
// 16 px from the edge (36 px items on a 40 px pitch, 3 x 16 accent bar), content 28 px to its right,
// a 28 px page title, 68 px cards 4 px apart with the icon centred 28 px in and the text 58 px in.
constexpr float kNavLeft = 16, kNavWidth = 280, kNavTop = 8, kNavItemH = 36, kNavPitch = 40;
constexpr float kContentGap = 28, kContentRight = 28, kTitleTop = 14, kContentTop = 76, kMaxContent = 1000;
constexpr float kCardH = 68, kCardGap = 4, kRowH = 56, kRadius = 4, kTextLeft = 58, kControlRight = 16;
constexpr float kSectionTop = 26, kSectionH = 20, kSectionBottom = 8, kPopupItemH = 36;

HWND g_wnd;
HINSTANCE g_inst;
fluent::Painter g_paint;
Provider g_provider;
Handler g_handler;
State g_state;
Page g_page = Page::Taskbar;
float g_scroll = 0, g_contentH = 0;
std::wstring g_expanded;  // device whose card is open
int g_stage = 99;         // build stage (WB_SETTINGS_STAGE), for checking the backdrop as parts are added

struct Hit {
    D2D1_RECT_F r;
    std::function<void()> click;
    bool overlay = false;
};
std::vector<Hit> g_hits;
POINT g_mouse{ -1, -1 };
bool g_down = false;
D2D1_RECT_F g_pressRect{ -1, -1, -1, -1 };

// The open combo box list.
struct Popup {
    bool open = false;
    D2D1_RECT_F anchor{};
    Choice choice;
    std::function<void(int)> pick;
} g_popup;

D2D1_RECT_F g_clip{};  // the scrolling content area

float ClientW() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    return rc.right / g_paint.scale;
}
float ClientH() {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    return rc.bottom / g_paint.scale;
}

bool Inside(const D2D1_RECT_F& r, POINT pt) {
    float x = pt.x / g_paint.scale, y = pt.y / g_paint.scale;
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

Look LookOf(const D2D1_RECT_F& r, bool enabled = true, bool overlay = false) {
    if (!enabled) return Look::Disabled;
    if (g_popup.open && !overlay) return Look::Rest;
    if (!Inside(r, g_mouse)) return Look::Rest;
    bool pressed = g_down && g_pressRect.left == r.left && g_pressRect.top == r.top;
    return pressed ? Look::Press : Look::Hover;
}

void AddHit(D2D1_RECT_F r, std::function<void()> click, bool clipped = true) {
    if (clipped) {
        r.top = std::max(r.top, g_clip.top);
        r.bottom = std::min(r.bottom, g_clip.bottom);
        if (r.bottom <= r.top) return;
    }
    g_hits.push_back({ r, std::move(click), false });
}

void Act(Action a, int value = 0, const std::wstring& key = L"") {
    if (g_handler) g_handler(a, value, key);
    Refresh();
}

// ---- cards ------------------------------------------------------------------------------------

struct Ctx {
    fluent::Palette c;
    float x, w;  // content column
    float y;     // next card's top (already scrolled)
};

void Section(Ctx& k, const std::wstring& title) {
    k.y += kSectionTop;
    g_paint.Text(title, fluent::kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, { k.x + 1, k.y, k.x + k.w, k.y + kSectionH }, k.c.text);
    k.y += kSectionH + kSectionBottom;
}

// Card background, icon, title and description; returns the card's rect.
D2D1_RECT_F CardBase(Ctx& k, wchar_t icon, const std::wstring& title, const std::wstring& desc, bool enabled,
                     float h = kCardH, Look look = Look::Rest) {
    D2D1_RECT_F r{ k.x, k.y, k.x + k.w, k.y + h };
    g_paint.Fill(r, kRadius, look == Look::Hover ? k.c.cardHover : look == Look::Press ? k.c.cardPressed : k.c.card);
    g_paint.Stroke(r, kRadius, k.c.cardStroke);
    float cy = r.top + kCardH / 2;
    D2D1_COLOR_F tc = enabled ? k.c.text : k.c.textDisabled;
    if (icon) g_paint.Glyph(icon, 20, { r.left + 16, cy - 14, r.left + 40, cy + 14 }, tc);
    float tl = icon ? r.left + kTextLeft : r.left + 16;
    if (desc.empty()) {
        g_paint.Text(title, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { tl, cy - 10, r.right - 260, cy + 10 }, tc);
    } else {
        g_paint.Text(title, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { tl, cy - 18, r.right - 260, cy + 2 }, tc);
        g_paint.Text(desc, fluent::kCaption, DWRITE_FONT_WEIGHT_NORMAL, { tl, cy + 2, r.right - 260, cy + 18 },
                     enabled ? k.c.textSecondary : k.c.textDisabled);
    }
    return r;
}

void ToggleCard(Ctx& k, wchar_t icon, const std::wstring& title, const std::wstring& desc, bool on,
                std::function<void()> flip, bool enabled = true) {
    D2D1_RECT_F r{ k.x, k.y, k.x + k.w, k.y + kCardH };
    Look look = LookOf(r, enabled);
    CardBase(k, icon, title, desc, enabled);
    float cy = r.top + kCardH / 2, right = r.right - kControlRight;
    g_paint.Text(on ? T(L"On") : T(L"Off"), fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { right - 160, cy - 10, right - 52, cy + 10 },
                 enabled ? k.c.text : k.c.textDisabled, 2);
    fluent::ToggleSwitch(g_paint, k.c, right, cy, on, look);
    if (enabled) AddHit(r, flip);
    k.y += kCardH + kCardGap;
}

void OpenPopup(D2D1_RECT_F anchor, const Choice& choice, std::function<void(int)> pick) {
    g_popup.open = true;
    g_popup.anchor = anchor;
    g_popup.choice = choice;
    g_popup.pick = std::move(pick);
}

// A combo box at the right of a card or row, centred on `cy`.
void Combo(Ctx& k, float right, float cy, const Choice& choice, std::function<void(int)> pick, bool enabled = true) {
    std::wstring longest;
    for (auto& o : choice.options)
        if (g_paint.TextWidth(o, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL) > g_paint.TextWidth(longest, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL))
            longest = o;
    float w = fluent::ComboWidth(g_paint, longest);
    D2D1_RECT_F r{ right - w, cy - 16, right, cy + 16 };
    std::wstring value = choice.selected >= 0 && choice.selected < (int)choice.options.size() ? choice.options[choice.selected] : L"";
    float keep = g_paint.alpha;
    if (!enabled) g_paint.alpha *= 0.45f;
    fluent::ComboBox(g_paint, k.c, r, value, LookOf(r, enabled));
    g_paint.alpha = keep;
    if (enabled) AddHit(r, [r, choice, pick] { OpenPopup(r, choice, pick); });
}

void ComboCard(Ctx& k, wchar_t icon, const std::wstring& title, const std::wstring& desc, const Choice& choice,
               std::function<void(int)> pick, bool enabled = true) {
    D2D1_RECT_F r = CardBase(k, icon, title, desc, enabled);
    Combo(k, r.right - kControlRight, r.top + kCardH / 2, choice, pick, enabled);
    k.y += kCardH + kCardGap;
}

void ButtonAt(Ctx& k, float right, float cy, const std::wstring& label, std::function<void()> click, bool accent = false) {
    float w = std::max(96.f, std::ceil(g_paint.TextWidth(label, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL)) + 24);
    D2D1_RECT_F r{ right - w, cy - 16, right, cy + 16 };
    fluent::Button(g_paint, k.c, r, label, LookOf(r), accent);
    AddHit(r, click);
}

void ButtonCard(Ctx& k, wchar_t icon, const std::wstring& title, const std::wstring& desc, const std::wstring& label,
                std::function<void()> click, bool accent = false) {
    D2D1_RECT_F r = CardBase(k, icon, title, desc, true);
    ButtonAt(k, r.right - kControlRight, r.top + kCardH / 2, label, click, accent);
    k.y += kCardH + kCardGap;
}

// ---- pages ------------------------------------------------------------------------------------

void TaskbarPage(Ctx& k) {
    const State& s = g_state;
    ComboCard(k, 0xE7F4, T(L"Show battery levels"), T(L"On the taskbar next to the clock, or as tray icons"), s.showAs,
              [](int i) { Act(Action::ShowAs, i); });
    ComboCard(k, 0xE790, T(L"Style"), T(L"How each device is drawn on the taskbar"), s.style,
              [](int i) { Act(Action::Style, i); }, s.widget);
    ComboCard(k, 0xE8E4, T(L"Position"), T(L"On the left needs Windows 11 with centred taskbar icons"), s.position,
              [](int i) { Act(Action::Position, i); }, s.widget);
    if (s.monitors.options.size() > 2)
        ComboCard(k, 0xE7F4, T(L"Show the widget on"), T(L"Which taskbars show the battery levels"), s.monitors,
                  [](int i) { Act(Action::Monitor, i); }, s.widget);
    Section(k, T(L"Appearance"));
    ToggleCard(k, 0xE790, T(L"Brand colors"), T(L"Razer green, Logitech blue and other makers' colors"), s.brandColors,
               [] { Act(Action::BrandColors, !g_state.brandColors); }, s.widget);
    ToggleCard(k, 0xE771, T(L"Background behind the text"), T(L"Keeps it readable on transparent taskbar themes"), s.background,
               [] { Act(Action::Background, !g_state.background); }, s.widget);
    Section(k, T(L"Sleeping and disconnected devices"));
    ToggleCard(k, 0xE708, T(L"Dim sleeping devices"), T(L"Show the last known level dimmed while a device sleeps"), s.dimSleeping,
               [] { Act(Action::DimSleeping, !g_state.dimSleeping); });
    ToggleCard(k, 0xED1A, T(L"Hide asleep devices"), T(L"Only show devices with a fresh reading"), s.hideAsleep,
               [] { Act(Action::HideAsleep, !g_state.hideAsleep); });
    ToggleCard(k, 0xE703, T(L"Show disconnected devices"), T(L"A dimmed cross for devices whose receiver reports them off"),
               s.showDisconnected, [] { Act(Action::ShowDisconnected, !g_state.showDisconnected); });
}

void DevicesPage(Ctx& k) {
    if (g_state.devices.empty()) {
        CardBase(k, fluent::glyph::Devices, T(L"No devices found yet"), T(L"Turn a device on or plug in its receiver."), true);
        k.y += kCardH + kCardGap;
        return;
    }
    for (const Device& d : g_state.devices) {
        bool open = d.key == g_expanded;
        float rows = 5;
        float h = kCardH + (open ? rows * kRowH : 0);
        D2D1_RECT_F head{ k.x, k.y, k.x + k.w, k.y + kCardH };
        Look look = LookOf(head);
        D2D1_RECT_F card = CardBase(k, fluent::DeviceGlyph(d.kind, d.bluetooth), d.name, d.detail, true, h,
                                    look == Look::Disabled ? Look::Rest : look);
        float cy = card.top + kCardH / 2, right = card.right - kControlRight;
        g_paint.Glyph(open ? fluent::glyph::ChevronUp : fluent::glyph::ChevronDown, 12, { right - 20, cy - 10, right, cy + 10 },
                      k.c.text);
        if (d.percent >= 0) {
            std::wstring level = std::to_wstring(d.percent) + L"%";
            float lw = g_paint.TextWidth(level, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL);
            g_paint.Text(level, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { right - 36 - lw - 2, cy - 10, right - 36, cy + 10 }, k.c.text, 2);
            g_paint.Glyph(fluent::BatteryGlyph(d.percent, d.charging), 16, { right - 36 - lw - 30, cy - 10, right - 36 - lw - 6, cy + 10 },
                          k.c.text);
        }
        std::wstring key = d.key;
        AddHit(head, [key] { g_expanded = g_expanded == key ? L"" : key; });
        if (open) {
            float y = card.top + kCardH;
            g_paint.Fill({ card.left + 1, y, card.right - 1, card.bottom - 1 }, 0, k.c.cardSecondary);
            auto row = [&](const std::wstring& title) {
                g_paint.Rect({ card.left + 1, y, card.right - 1, y + 1 }, k.c.divider);
                g_paint.Text(title, fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { card.left + kTextLeft, y, card.right - 320, y + kRowH },
                             k.c.text);
                float rcy = y + kRowH / 2;
                y += kRowH;
                return rcy;
            };
            float rcy = row(T(L"Show on the taskbar"));
            {
                D2D1_RECT_F tr{ right - 40, rcy - 12, right, rcy + 12 };
                g_paint.Text(d.shown ? T(L"On") : T(L"Off"), fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { right - 160, rcy - 10, right - 52, rcy + 10 },
                             k.c.text, 2);
                fluent::ToggleSwitch(g_paint, k.c, right, rcy, d.shown, LookOf(tr));
                AddHit(tr, [key] { Act(Action::DeviceShown, 0, key); });
            }
            rcy = row(T(L"Order on the taskbar"));
            {
                float bw = 96, gap = 8;
                D2D1_RECT_F rb{ right - bw, rcy - 16, right, rcy + 16 }, lb{ right - 2 * bw - gap, rcy - 16, right - bw - gap, rcy + 16 };
                float keep = g_paint.alpha;
                if (d.first) g_paint.alpha *= .45f;
                fluent::Button(g_paint, k.c, lb, T(L"Move left"), LookOf(lb, !d.first));
                g_paint.alpha = keep;
                if (d.last) g_paint.alpha *= .45f;
                fluent::Button(g_paint, k.c, rb, T(L"Move right"), LookOf(rb, !d.last));
                g_paint.alpha = keep;
                if (!d.first) AddHit(lb, [key] { Act(Action::DeviceLeft, 0, key); });
                if (!d.last) AddHit(rb, [key] { Act(Action::DeviceRight, 0, key); });
            }
            rcy = row(T(L"Name"));
            {
                float bw = 96;
                D2D1_RECT_F rn{ right - bw, rcy - 16, right, rcy + 16 };
                fluent::Button(g_paint, k.c, rn, T(L"Rename"), LookOf(rn));
                AddHit(rn, [key] { Act(Action::DeviceRename, 0, key); });
                if (d.renamed) {
                    D2D1_RECT_F rr{ right - 2 * bw - 8, rcy - 16, right - bw - 8, rcy + 16 };
                    fluent::Button(g_paint, k.c, rr, T(L"Reset"), LookOf(rr));
                    AddHit(rr, [key] { Act(Action::DeviceResetName, 0, key); });
                }
            }
            rcy = row(T(L"Low battery notification"));
            Combo(k, right, rcy, d.alert, [key](int i) { Act(Action::DeviceAlert, i, key); });
            rcy = row(T(L"Battery history"));
            {
                D2D1_RECT_F rh{ right - 96, rcy - 16, right, rcy + 16 };
                fluent::Button(g_paint, k.c, rh, T(L"Open"), LookOf(rh));
                AddHit(rh, [key] { Act(Action::DeviceHistory, 0, key); });
            }
        }
        k.y += h + kCardGap;
    }
}

void GeneralPage(Ctx& k) {
    const State& s = g_state;
    ToggleCard(k, 0xE768, T(L"Start with Windows"), T(L"Open BatteryHub when you sign in"), s.autostart,
               [] { Act(Action::Autostart, !g_state.autostart); });
    ComboCard(k, 0xE72C, T(L"Check battery every"), T(L"Longer intervals use less power"), s.poll, [](int i) { Act(Action::Poll, i); });
    ToggleCard(k, fluent::glyph::History, T(L"Record battery history"), T(L"Keep readings for the battery history charts"),
               s.recordHistory, [] { Act(Action::RecordHistory, !g_state.recordHistory); });
    ComboCard(k, 0xE771, T(L"Panel background"), T(L"Mica and Acrylic are Windows' materials; Clear glass is more transparent"),
              s.panel, [](int i) { Act(Action::Panel, i); });
    Section(k, T(L"Language"));
    ComboCard(k, 0xF2B7, T(L"Language"), T(L"Automatic follows the Windows display language"), s.language,
              [](int i) { Act(Action::Language, i); });
    ButtonCard(k, 0xE8F4, T(L"Add your own language"), T(L"Opens the folder for your language files"), T(L"Open folder"),
               [] { Act(Action::AddLanguage); });
    Section(k, T(L"BatteryHub"));
    ButtonCard(k, fluent::glyph::Refresh, T(L"Check batteries now"), T(L"Reads every device right away"), T(L"Refresh"),
               [] { Act(Action::Refresh); });
    ButtonCard(k, 0xE7E8, T(L"Exit BatteryHub"), T(L"Closes the taskbar widget until you open it again"), T(L"Exit"),
               [] { Act(Action::Exit); });
}

void AboutPage(Ctx& k) {
    CardBase(k, 0xE946, L"BatteryHub " + g_state.version, T(L"A personal continuation of BatteryHub by krisgfx9, GPL-3.0"), true);
    k.y += kCardH + kCardGap;
    ButtonCard(k, 0xE774, T(L"Project page"), L"github.com/wachi35676/open-battery", T(L"Open"), [] { Act(Action::OpenProject); });
}

// ---- frame ------------------------------------------------------------------------------------

struct NavItem { Page page; wchar_t glyph; const wchar_t* label; };
const NavItem kNav[] = { { Page::Taskbar, 0xE7F4, L"Taskbar" }, { Page::Devices, 0xE772, L"Devices" },
                         { Page::General, 0xE713, L"General" }, { Page::About, 0xE946, L"About" } };

const wchar_t* PageTitle(Page p) {
    for (auto& n : kNav)
        if (n.page == p) return n.label;
    return L"";
}

void DrawPopup(const fluent::Palette& c) {
    const Choice& ch = g_popup.choice;
    float w = std::max(g_popup.anchor.right - g_popup.anchor.left, 140.f);
    float h = ch.options.size() * kPopupItemH + 8;
    // WinUI opens the list with the selected item over the combo box.
    float top = g_popup.anchor.top - 4 - std::max(0, ch.selected) * kPopupItemH - (kPopupItemH - 32) / 2;
    top = std::clamp(top, 8.f, std::max(8.f, ClientH() - h - 8));
    D2D1_RECT_F r{ g_popup.anchor.left - 4, top, g_popup.anchor.left - 4 + w + 8, top + h };
    for (int i = 3; i >= 1; i--)  // soft shadow
        g_paint.Fill({ r.left - (float)i, r.top - (float)i + 2, r.right + (float)i, r.bottom + (float)i + 3 }, 8.f + i, D2D1::ColorF(0, 0, 0, 0.06f));
    g_paint.Fill(r, 8, c.popup);
    g_paint.Stroke(r, 8, c.popupStroke);
    for (size_t i = 0; i < ch.options.size(); i++) {
        D2D1_RECT_F it{ r.left + 4, r.top + 4 + i * kPopupItemH, r.right - 4, r.top + 4 + (i + 1) * kPopupItemH - 2 };
        Look look = LookOf(it, true, true);
        bool sel = (int)i == ch.selected;
        if (sel || look != Look::Rest) g_paint.Fill(it, 4, look == Look::Press ? c.subtlePressed : c.subtleHover);
        if (sel) g_paint.Fill({ it.left, (it.top + it.bottom) / 2 - 8, it.left + 3, (it.top + it.bottom) / 2 + 8 }, 1.5f, c.accent);
        g_paint.Text(ch.options[i], fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { it.left + 12, it.top, it.right - 8, it.bottom }, c.text);
        int index = (int)i;
        g_hits.push_back({ it, [index] {
                               auto pick = g_popup.pick;
                               g_popup.open = false;
                               if (pick) pick(index);
                           },
                           true });
    }
}

void Draw() {
    if (!g_paint.Begin(g_wnd)) return;
    fluent::Palette c = fluent::Colors(g_state.light);
    g_hits.clear();
    float W = ClientW(), H = ClientH();

    if (g_stage >= 1) {  // navigation
        for (size_t i = 0; i < sizeof(kNav) / sizeof(kNav[0]); i++) {
            const NavItem& n = kNav[i];
            D2D1_RECT_F r{ kNavLeft, kNavTop + i * kNavPitch, kNavLeft + kNavWidth, kNavTop + i * kNavPitch + kNavItemH };
            Look look = LookOf(r);
            bool sel = n.page == g_page;
            if (sel || look != Look::Rest)
                g_paint.Fill(r, kRadius, look == Look::Press ? c.subtlePressed : c.subtleHover);
            if (sel) {
                float cy = (r.top + r.bottom) / 2;
                g_paint.Fill({ r.left, cy - 8, r.left + 3, cy + 8 }, 1.5f, c.accent);
            }
            float cy = (r.top + r.bottom) / 2;
            g_paint.Glyph(n.glyph, 16, { r.left + 10, cy - 12, r.left + 34, cy + 12 }, c.text);
            g_paint.Text(T(n.label), fluent::kBody, DWRITE_FONT_WEIGHT_NORMAL, { r.left + 45, r.top, r.right - 8, r.bottom }, c.text);
            Page page = n.page;
            AddHit(r, [page] { g_page = page; g_scroll = 0; g_expanded.clear(); }, false);
        }
    }

    float x = kNavLeft + kNavWidth + kContentGap;
    float w = std::min(kMaxContent, W - x - kContentRight);
    if (g_stage >= 2)
        g_paint.Text(T(PageTitle(g_page)), fluent::kTitle, DWRITE_FONT_WEIGHT_SEMI_BOLD, { x, kTitleTop, x + w, kTitleTop + 44 }, c.text);

    g_clip = { x - 8, kContentTop - 4, W, H };
    if (g_stage >= 3 && w > 100) {
        g_paint.Clip(g_clip);
        Ctx k{ c, x, w, kContentTop - g_scroll };
        float start = k.y;
        switch (g_page) {
            case Page::Taskbar: TaskbarPage(k); break;
            case Page::Devices: DevicesPage(k); break;
            case Page::General: GeneralPage(k); break;
            case Page::About: AboutPage(k); break;
        }
        g_contentH = k.y - start + 24;
        g_paint.Unclip();
    }
    if (g_popup.open) DrawPopup(c);
    g_paint.End();
}

void ClampScroll() {
    float view = ClientH() - kContentTop;
    g_scroll = std::clamp(g_scroll, 0.f, std::max(0.f, g_contentH - view));
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
        case WM_SIZE:
            g_paint.Resize(LOWORD(lp), HIWORD(lp));
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_DPICHANGED: {
            g_paint.scale = HIWORD(wp) / 96.f;
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            float s = fluent::WindowDpi(h) / 96.f;
            mm->ptMinTrackSize = { (LONG)(720 * s), (LONG)(500 * s) };
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (!g_popup.open) {
                g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48;
                ClampScroll();
                InvalidateRect(h, nullptr, FALSE);
            }
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
        case WM_LBUTTONDOWN: {
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            g_down = true;
            g_pressRect = { -1, -1, -1, -1 };
            for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
                if ((!g_popup.open || it->overlay) && Inside(it->r, pt)) { g_pressRect = it->r; break; }
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            ReleaseCapture();
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            g_down = false;
            std::function<void()> click;
            bool hitOverlay = false;
            for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
                if (g_popup.open && !it->overlay) continue;
                if (Inside(it->r, pt) && it->r.left == g_pressRect.left && it->r.top == g_pressRect.top) {
                    click = it->click;
                    hitOverlay = it->overlay;
                    break;
                }
            }
            if (g_popup.open && !hitOverlay) g_popup.open = false;  // a click outside the list closes it
            if (click) click();
            ClampScroll();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && g_popup.open) {
                g_popup.open = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE && g_popup.open) {
                g_popup.open = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g_wnd = nullptr;
            g_popup.open = false;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Show(HINSTANCE inst, Page page, Provider provider, Handler handler) {
    g_provider = std::move(provider);
    g_handler = std::move(handler);
    g_page = page;
    g_scroll = 0;
    g_popup.open = false;
    if (g_provider) g_state = g_provider();
    if (g_wnd) {
        if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
        SetForegroundWindow(g_wnd);
        InvalidateRect(g_wnd, nullptr, FALSE);
        return;
    }
    if (const char* s = getenv("WB_SETTINGS_STAGE")) g_stage = atoi(s);
    g_inst = inst;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"WirelessBatterySettings";
        RegisterClassExW(&wc);
        registered = true;
    }
    // 1000 x 700 DIPs, centred on the monitor under the cursor.
    POINT pt;
    GetCursorPos(&pt);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_wnd = CreateWindowExW(0, L"WirelessBatterySettings", (std::wstring(L"BatteryHub ") + T(L"settings")).c_str(),
                            WS_OVERLAPPEDWINDOW, mi.rcWork.left, mi.rcWork.top, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!g_wnd) return;
    g_paint.scale = fluent::WindowDpi(g_wnd) / 96.f;
    int w = std::min((int)(1000 * g_paint.scale), (int)(mi.rcWork.right - mi.rcWork.left));
    int h = std::min((int)(700 * g_paint.scale), (int)(mi.rcWork.bottom - mi.rcWork.top));
    SetWindowPos(g_wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    fluent::ApplyBackdrop(g_wnd, g_state.light, fluent::Backdrop::Mica);
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    UpdateWindow(g_wnd);
    fluent::ApplyBackdrop(g_wnd, g_state.light, fluent::Backdrop::Mica);
}

void Refresh() {
    if (!g_wnd || !g_provider) return;
    bool light = g_state.light;
    g_state = g_provider();
    if (light != g_state.light) fluent::ApplyBackdrop(g_wnd, g_state.light, fluent::Backdrop::Mica);
    InvalidateRect(g_wnd, nullptr, FALSE);
}

bool Visible() { return g_wnd && IsWindowVisible(g_wnd); }

void Close() {
    if (g_wnd) DestroyWindow(g_wnd);
}

}  // namespace settings_window
