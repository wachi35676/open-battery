#include "widget.h"

#include "fluent.h"
#include "i18n.h"
#include "widget_icons.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <tuple>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light, g_hover;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
bool g_vertical;    // the primary taskbar is docked left or right: one block per device, stacked (PR #321)
bool g_left;        // left end of a centred Windows 11 taskbar instead of next to the tray (menu option)
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
SIZE g_size{};
HINSTANCE g_inst;

// The charging pulse: every two seconds a dot of light runs along the cable of each charging device, as in
// the click panel. Two timers on the primary strip's window: a slow one between pulses and a 15 fps one
// during a pulse, so nothing is repainted in between. Neither runs unless a shown device is charging, and
// with Windows animations turned off there is no pulse at all (the cable and its plug stay as drawn).
constexpr UINT_PTR kPulseIdle = 1, kPulseFrame = 2;
constexpr int kPulseEveryMs = 2000, kPulseMs = 660, kPulseFrameMs = 66;
bool g_pulseOn;           // the idle or the frame timer is running
double g_pulse = -1;      // 0..1 along the cable during a pulse, otherwise -1
ULONGLONG g_pulseStart;

// Copies of the strip on secondary-monitor taskbars (Shell_SecondaryTrayWnd), so the levels stay visible
// while a game runs fullscreen on the main screen. Same drawing, same click and tooltip; each one sits
// left of its taskbar's clock. The primary strip above is unchanged, and with one monitor none exist.
struct Mirror {
    HWND bar = nullptr, wnd = nullptr;
    bool hover = false;
    SIZE size{};
    int dpi = 0;
    int lastLeft = INT_MIN;  // client x it was last placed at; INT_MIN = not placed (hidden)
};
std::vector<Mirror> g_mirrors;
// Which taskbars show the strip: "all", "main", or one monitor's id (MonitorId), e.g. only monitor 3.
// With one monitor chosen, the primary strip is hidden; if that monitor is not connected, the primary
// strip is shown instead so the levels never disappear.
std::wstring g_choice = L"all";
bool g_primaryHidden = false;
HWND g_dpiBar;  // while a mirror is painted or placed, Px() uses its taskbar's DPI

const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi() {
    HWND bar = g_dpiBar ? g_dpiBar : g_taskbar;
    UINT d = bar ? GetDpiForWindow(bar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on BatteryHub.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

// Where a Windows 11 secondary taskbar's clock, widgets button and task buttons are. Its XAML has no child
// windows for them, so the same lookup thread reads them with UI Automation (the clock has AutomationId
// "SystemTrayIcon", widgets button "WidgetsButton", task buttons class "Taskbar.TaskListButtonAutomationPeer";
// found by PR #107).
struct Span {
    HWND bar = nullptr;
    bool measured = false;   // UI Automation answered for this taskbar
    bool hasClock = false;   // the clock can be turned off on secondary taskbars
    int clockLeft = 0;       // screen x
    bool hasWidgets = false; // Widgets (weather) button on secondary taskbar
    RECT widgets{};          // bounding rectangle of WidgetsButton (screen coords)
    bool hasTasks = false;
    int tasksRight = 0;      // right edge of the last task button left of the clock (screen x)
    bool hasStart = false;   // Start button (for the left placement)
    int startLeft = 0;       // screen x
    int widgetsRight = 0;    // right edge of the Widgets button, 0 when there is none (screen x)
};
std::vector<Span> g_spans;   // guarded by g_wbLock
bool g_wbMirrors;            // guarded by g_wbLock: measure secondary taskbars too
bool g_wbLeft;               // guarded by g_wbLock: measure the main taskbar for the left placement
Span g_primarySpan;          // guarded by g_wbLock: the main taskbar, while g_wbLeft is set

IUIAutomationCondition* StringCondition(IUIAutomation* uia, PROPERTYID prop, const wchar_t* value) {
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = SysAllocString(value);
    IUIAutomationCondition* c = nullptr;
    if (v.bstrVal) uia->CreatePropertyCondition(prop, v, &c);
    VariantClear(&v);
    return c;
}

Span MeasureSpan(IUIAutomation* uia, HWND bar) {
    Span s;
    s.bar = bar;
    RECT br{};
    IUIAutomationElement* root = nullptr;
    if (!GetWindowRect(bar, &br) || FAILED(uia->ElementFromHandle(bar, &root)) || !root) return s;
    s.measured = true;
    auto inBar = [&](const RECT& r) { return r.right > r.left && r.left >= br.left && r.right <= br.right; };
    if (IUIAutomationCondition* clock = StringCondition(uia, UIA_AutomationIdPropertyId, L"SystemTrayIcon")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, clock, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {  // leftmost of the clock area's buttons
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.left < s.clockLeft)) {
                    s.clockLeft = r.left;
                    s.hasClock = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        clock->Release();
    }
    if (IUIAutomationCondition* wbCond = StringCondition(uia, UIA_AutomationIdPropertyId, L"WidgetsButton")) {
        IUIAutomationElement* wbEl = nullptr;
        RECT r{};
        if (SUCCEEDED(root->FindFirst(TreeScope_Descendants, wbCond, &wbEl)) && wbEl &&
            SUCCEEDED(wbEl->get_CurrentBoundingRectangle(&r)) && inBar(r)) {
            s.widgets = r;
            s.hasWidgets = true;
        }
        if (wbEl) wbEl->Release();
        wbCond->Release();
    }
    if (IUIAutomationCondition* task = StringCondition(uia, UIA_ClassNamePropertyId, L"Taskbar.TaskListButtonAutomationPeer")) {
        IUIAutomationElementArray* all = nullptr;
        int n = 0;
        if (SUCCEEDED(root->FindAll(TreeScope_Descendants, task, &all)) && all && SUCCEEDED(all->get_Length(&n)))
            for (int i = 0; i < n; i++) {
                IUIAutomationElement* el = nullptr;
                RECT r{};
                if (SUCCEEDED(all->GetElement(i, &el)) && el && SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r) &&
                    (!s.hasClock || r.right <= s.clockLeft) && (!s.hasWidgets || r.right <= s.widgets.left) &&
                    (!s.hasTasks || r.right > s.tasksRight)) {
                    s.tasksRight = r.right;
                    s.hasTasks = true;
                }
                if (el) el->Release();
            }
        if (all) all->Release();
        task->Release();
    }
    auto first = [&](const wchar_t* id, RECT& r) {
        IUIAutomationCondition* c = StringCondition(uia, UIA_AutomationIdPropertyId, id);
        IUIAutomationElement* el = nullptr;
        bool ok = c && SUCCEEDED(root->FindFirst(TreeScope_Descendants, c, &el)) && el &&
                  SUCCEEDED(el->get_CurrentBoundingRectangle(&r)) && inBar(r);
        if (el) el->Release();
        if (c) c->Release();
        return ok;
    };
    RECT r{};
    if (first(L"StartButton", r)) s.hasStart = true, s.startLeft = r.left;
    if (first(L"WidgetsButton", r)) s.widgetsRight = r.right;
    root->Release();
    return s;
}

// Left placement: the free room between the taskbar's left edge (or the Widgets button, when it sits
// there) and the Start button. Only a Windows 11 taskbar with centred icons has room there; with
// left-aligned icons the Start button is at the very left, nothing fits, and the strip stays right.
bool LeftRoom(const Span& s, const RECT& bar, int widgetsRight, int pad, int& from, int& to) {
    if (!s.measured || !s.hasStart) return false;
    from = bar.left + pad;
    if (widgetsRight > from && widgetsRight < s.startLeft) from = widgetsRight + pad;
    to = s.startLeft - pad;
    return to > from;
}

bool IsXamlTaskbar(HWND bar) {
    return FindWindowExW(bar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
}

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockShared(&g_wbLock);
        bool mirrors = g_wbMirrors, left = g_wbLeft;
        ReleaseSRWLockShared(&g_wbLock);
        Span primary;
        if (bar && left && uia && IsXamlTaskbar(bar)) primary = MeasureSpan(uia, bar);
        std::vector<Span> spans;
        if (bar && mirrors && uia)
            for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
                 t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr))
                if (IsXamlTaskbar(t)) spans.push_back(MeasureSpan(uia, t));  // Windows 10 bars have real child windows
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        g_spans = std::move(spans);
        g_primarySpan = primary;
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

constexpr wchar_t kCheck = 0xE73E;  // CheckMark after the level once fully charged (still on power)

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
    int base = 0;  // rows from the top of the ink down to the text's baseline
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    m.base = m.h;
    return m;
}

// Text as the taskbar's own clock draws it: Segoe UI Variable through DirectWrite with greyscale
// anti-aliasing, `px` pixels high. `tabular` asks for fixed-width digits, so a level that changes does not
// make the strip shift about. Cropped to the ink, with the baseline kept so that pieces of different sizes
// can share a line. Cached: the strip repaints every second and shows the same few strings.
Mask TextMask(const std::wstring& s, int px, bool tabular) {
    static std::map<std::tuple<std::wstring, int, bool>, Mask> cache;
    auto key = std::make_tuple(s, px, tabular);
    auto hit = cache.find(key);
    if (hit != cache.end()) return hit->second;
    if (cache.size() > 400) cache.clear();
    Mask m;
    static ID2D1Factory* d2d = nullptr;
    static ID2D1DCRenderTarget* rt = nullptr;
    IDWriteFactory* dw = fluent::DWrite();
    if (!d2d) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d);
    if (d2d && !rt) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96, 96);
        d2d->CreateDCRenderTarget(&props, &rt);
    }
    IDWriteTextFormat* format = nullptr;
    IDWriteTextLayout* layout = nullptr;
    if (dw && rt && !s.empty() &&
        SUCCEEDED(dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, (float)px, L"", &format)) &&
        SUCCEEDED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), format, 4000, 400, &layout))) {
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        IDWriteTypography* typography = nullptr;
        if (tabular && SUCCEEDED(dw->CreateTypography(&typography))) {
            DWRITE_FONT_FEATURE feature = { DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1 };
            DWRITE_TEXT_RANGE all = { 0, (UINT32)s.size() };
            typography->AddFontFeature(feature);
            layout->SetTypography(typography, all);
            typography->Release();
        }
        DWRITE_TEXT_METRICS tm{};
        DWRITE_LINE_METRICS lm{};
        UINT32 lines = 0;
        layout->GetMetrics(&tm);
        layout->GetLineMetrics(&lm, 1, &lines);
        const int pad = 4;
        int W = (int)std::ceil(tm.widthIncludingTrailingWhitespace) + 2 * pad, H = (int)std::ceil(tm.height) + 2 * pad;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) {
            HGDIOBJ ob = SelectObject(dc, bmp);
            RECT rc{ 0, 0, W, H };
            ID2D1SolidColorBrush* white = nullptr;
            if (SUCCEEDED(rt->BindDC(dc, &rc))) {
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0, 0, 0, 1));
                rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
                if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white))) {
                    rt->DrawTextLayout(D2D1::Point2F((float)pad, (float)pad), layout, white);
                    white->Release();
                }
                if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) {
                    rt->Release();
                    rt = nullptr;
                }
                GdiFlush();
                const DWORD* p = static_cast<const DWORD*>(bits);
                int x0 = W, y0 = H, x1 = -1, y1 = -1;
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++)
                        if ((p[y * W + x] >> 8) & 0xFF) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
                if (x1 >= 0) {
                    m.w = x1 - x0 + 1;
                    m.h = y1 - y0 + 1;
                    m.a.resize((size_t)m.w * m.h);
                    for (int y = 0; y < m.h; y++)
                        for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = (BYTE)((p[(y0 + y) * W + x0 + x] >> 8) & 0xFF);
                    m.base = (int)std::lround(pad + lm.baseline) - y0;
                }
            }
            SelectObject(dc, ob);
            DeleteObject(bmp);
        }
        DeleteDC(dc);
    }
    if (layout) layout->Release();
    if (format) format->Release();
    if (!m.w) {  // DirectWrite or Direct2D unavailable: GDI
        HFONT font = MakeFont(px, FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
        m = RenderMask(s, font, g_light);
        DeleteObject(font);
    }
    cache[key] = m;
    return m;
}

// "85%" as the level: the number in the clock's size with fixed-width digits, and a smaller, quieter
// percent sign on the same baseline, so the number is what the eye lands on.
Mask LevelMask(int percent, int numberPx, int signPx) {
    Mask d = TextMask(std::to_wstring(percent), numberPx, true), p = TextMask(L"%", signPx, false);
    int gap = std::max(1, numberPx / 12);
    Mask m;
    m.base = std::max(d.base, p.base);
    m.w = d.w + gap + p.w;
    m.h = m.base + std::max(d.h - d.base, p.h - p.base);
    m.a.assign((size_t)m.w * m.h, 0);
    for (int y = 0; y < d.h; y++)
        for (int x = 0; x < d.w; x++) m.a[(size_t)(y + m.base - d.base) * m.w + x] = d.a[(size_t)y * d.w + x];
    for (int y = 0; y < p.h; y++)
        for (int x = 0; x < p.w; x++)
            m.a[(size_t)(y + m.base - p.base) * m.w + d.w + gap + x] = (BYTE)(p.a[(size_t)y * p.w + x] * 66 / 100);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
    void Draw(const widget_icons::Sprite& s, int x, int y, int alpha) {  // premultiplied source over
        for (int j = 0; j < s.h; j++)
            for (int i = 0; i < s.w; i++) {
                int X = x + i, Y = y + j;
                DWORD v = s.px[(size_t)j * s.w + i];
                if (X < 0 || Y < 0 || X >= w || Y >= h || !(v >> 24)) continue;
                DWORD d = px[Y * w + X];
                int inv = 255 - (int)(v >> 24) * alpha / 255;
                DWORD o = 0;
                for (int sh = 0; sh < 32; sh += 8)
                    o |= (DWORD)std::min(255, (int)((v >> sh) & 255) * alpha / 255 + (int)((d >> sh) & 255) * inv / 255) << sh;
                px[Y * w + X] = o;
            }
    }
};

// One drawn piece of the strip: a device icon (`icon`) or a text / glyph mask (`m`). Normal pieces follow
// each other from left to right. A piece with an `anchor` (index of an earlier piece) is a caption: drawn
// over that piece, `bx`/`by` from its top-left corner, and taking no room of its own.
struct Piece {
    Mask m;
    const widget_icons::Sprite* icon = nullptr;
    int w = 0, h = 0;
    Rgb color{};
    int alpha = 255;
    int gapAfter = 0;
    int anchor = -1;
    int bx = 0, by = 0;
    int px = INT_MIN, py = 0;  // fixed top-left position (vertical layout); INT_MIN = flow left to right
    int lift = 0;              // row layout: drawn this much above centre (an icon with a caption under it)
    int below = 0;             // vertical layout: extra room under the piece (the caption's)
    bool pulse = false;        // a charging device's icon: the pulse runs along its cable
};

Piece TextPiece(Mask m, Rgb color, int alpha, int gapAfter) {
    Piece p;
    p.w = m.w;
    p.h = m.h;
    p.m = std::move(m);
    p.color = color;
    p.alpha = alpha;
    p.gapAfter = gapAfter;
    return p;
}

Piece IconPiece(const widget_icons::Sprite& s, int alpha, int gapAfter) {
    Piece p;
    p.icon = &s;
    p.w = s.w;
    p.h = s.h;
    p.alpha = alpha;
    p.gapAfter = gapAfter;
    return p;
}

// The one style of the strip: a small isometric icon of each device (widget_icons) with its level.
//  * horizontal taskbar: "[icon] 30%  [icon] 93%" in one row
//  * vertical (left / right taskbar, from PR #321): one block per device, the icon above its level
// The icon carries the state too: a cable plugged into it while on power, its plug blue while charging
// (with a pulse of light now and then) and green once full, then also a check mark after the level. Low
// shows as a red level, and a device that is asleep is faded. `width` and `height` return the strip's
// size; in the vertical layout the width is the bar's.
std::vector<Piece> Layout(int barW, int barH, int& width, int& height) {
    using widget_icons::State;
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 43, 28 } : Rgb{ 255, 153, 164 };  // Windows' critical text colour
    Rgb full = g_light ? Rgb{ 16, 124, 16 } : Rgb{ 108, 203, 95 };  // Windows "success" green
    HFONT check = MakeFont(Px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT caption = MakeFont(std::max(8, Px(7)), FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");  // never under 8 px
    // Icon height: 20 px at 100%, between the tray's icons and the apps', and smaller on a small taskbar.
    // Resolved here, per strip, because a mirror on another monitor can have another DPI. The level is set
    // like the clock: 12 px.
    int iconSize = std::max(Px(12), std::min(Px(20), (g_vertical ? barW : barH) - Px(10)));
    const int textPx = Px(12), signPx = Px(10);

    std::vector<Piece> out;
    struct Slot { int icon, label, bolt; };  // indices of one device's pieces
    std::vector<Slot> slots;
    if (g_items.empty())  // nothing to show yet: a faded battery cell
        out.push_back(IconPiece(widget_icons::Get(DeviceKind::Other, State::Normal, g_light, iconSize), 110, 0));
    for (size_t i = 0; i < g_items.size(); i++) {
        const auto& it = g_items[i];
        int a = it.dim ? 120 : 255;
        bool mark = it.full;  // green check mark once full; charging shows as the cable alone
        State state = it.full ? State::Full : it.charging ? State::Charging : it.low ? State::Low : State::Normal;
        size_t icon = out.size();
        slots.push_back({ (int)icon, (int)icon + 1, mark ? (int)icon + 2 : -1 });
        out.push_back(IconPiece(widget_icons::Get(it.kind, state, g_light, iconSize), a, Px(6)));
        out.back().pulse = state == State::Charging && !it.dim;
        Mask level = !it.text.empty() ? TextMask(it.text, textPx, false)
                     : it.percent >= 0 ? LevelMask(it.percent, textPx, signPx) : TextMask(L"?", textPx, false);
        out.push_back(TextPiece(std::move(level), it.low ? red : fg, a, mark ? Px(3) : 0));
        if (mark) out.push_back(TextPiece(RenderMask(std::wstring(1, kCheck), check, g_light), full, a, 0));
        out.back().gapAfter = i + 1 < g_items.size() ? Px(14) : 0;
        if (!it.label.empty()) {
            // Tiny caption centred under the icon, the pair centred in the bar, so the caption never hides
            // the drawing. A taskbar too low for that gets it over the icon's bottom-right corner instead.
            Piece c = TextPiece(RenderMask(it.label, caption, g_light), fg, a, 0);
            c.anchor = (int)icon;
            int room = c.h + Px(1);
            if (g_vertical || barH >= out[icon].icon->bodyH + room + Px(8)) {
                c.bx = (out[icon].w - c.w) / 2;
                c.by = out[icon].h + Px(1);
                out[icon].lift = room / 2;
                out[icon].below = room;
            } else {
                c.bx = out[icon].w - c.w + Px(2);
                c.by = out[icon].h - c.h + Px(2);
            }
            out.push_back(std::move(c));
        }
    }
    DeleteObject(check);
    DeleteObject(caption);

    height = barH;
    if (g_vertical) {
        // One block per device, centred in the bar's width: icon, then the level (and bolt) under it.
        width = barW;
        int y = Px(6);
        if (slots.empty()) {
            // No devices yet (startup, or the last one disconnected): only the placeholder icon. Without this
            // branch the row layout below would keep height = barH, and on a left / right taskbar barH is the
            // whole screen height, so the strip covered the entire taskbar.
            Piece& ic = out[0];
            ic.px = (barW - ic.w) / 2;
            ic.py = y;
            height = y + ic.h + Px(6);
            return out;
        }
        for (const Slot& s : slots) {
            Piece& ic = out[s.icon];
            Piece& lb = out[s.label];
            int boltW = s.bolt >= 0 ? Px(2) + out[s.bolt].w : 0;
            ic.px = (barW - ic.w) / 2;
            ic.py = y;
            y += ic.h + ic.below + Px(3);
            lb.px = (barW - lb.w - boltW) / 2;
            lb.py = y;
            if (s.bolt >= 0) {
                out[s.bolt].px = lb.px + lb.w + Px(2);
                out[s.bolt].py = y + (lb.h - out[s.bolt].h) / 2;
            }
            y += lb.h + Px(10);
        }
        height = y - Px(10) + Px(6);
        return out;
    }
    width = Px(8) * 2;
    for (auto& p : out)
        if (p.anchor < 0) width += p.w + p.gapAfter;
    return out;
}

// Draws the strip for one window: the primary one or a mirror on a secondary taskbar.
void PaintStrip(HWND wnd, HWND taskbar, bool hover, SIZE& size) {
    RECT bar;
    GetClientRect(taskbar, &bar);
    int W = 0, H = 0;
    auto pieces = Layout(bar.right - bar.left, bar.bottom - bar.top, W, H);
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        for (int y = inset; y < H - inset; y++)
            for (int x = 0; x < W; x++) {
                int dx = x < r ? r - x : (x >= W - r ? x - (W - r - 1) : 0);
                int dy = y < inset + r ? inset + r - y : (y >= H - inset - r ? y - (H - inset - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    Rgb barColor = g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 };
    if (g_background) roundRect(Px(4), Px(6), barColor, 240);
    if (hover) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 12 : 18);  // as the tray's own hover

    int x = Px(8);
    std::vector<int> xs(pieces.size()), ys(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++) {
        const Piece& p = pieces[i];
        if (p.anchor < 0) {
            bool fixed = p.px != INT_MIN;  // vertical layout: Layout() placed it
            xs[i] = fixed ? p.px : x;
            // An icon is centred on the device, not on the device plus its cable, so plugging in moves nothing.
            ys[i] = fixed ? p.py : p.icon ? (H - p.icon->bodyH) / 2 - p.icon->bodyY - p.lift : (H - p.h) / 2;
            if (!fixed) x += p.w + p.gapAfter;
        } else {  // caption: one-pixel outline in the taskbar's colour so it reads over the icon
            xs[i] = xs[p.anchor] + p.bx;
            ys[i] = ys[p.anchor] + p.by;
            for (int oy = -1; oy <= 1; oy++)
                for (int ox = -1; ox <= 1; ox++)
                    if (ox || oy) cv.Draw(p.m, xs[i] + ox, ys[i] + oy, barColor, 255);
        }
        if (p.icon) cv.Draw(*p.icon, xs[i], ys[i], p.alpha);
        else cv.Draw(p.m, xs[i], ys[i], p.color, p.alpha);
        if (p.icon && p.pulse && g_pulse >= 0 && p.icon->cable.size() >= 2) {
            // The pulse: a soft dot of light at g_pulse along the cable, fading in and out at its ends.
            const auto& path = p.icon->cable;
            double total = 0;
            for (size_t k = 1; k < path.size(); k++) total += std::hypot(path[k].x - path[k - 1].x, path[k].y - path[k - 1].y);
            double want = g_pulse * total, cx = path.back().x, cy = path.back().y;
            for (size_t k = 1; k < path.size(); k++) {
                double seg = std::hypot(path[k].x - path[k - 1].x, path[k].y - path[k - 1].y);
                if (want <= seg && seg > 0) {
                    cx = path[k - 1].x + (path[k].x - path[k - 1].x) * want / seg;
                    cy = path[k - 1].y + (path[k].y - path[k - 1].y) * want / seg;
                    break;
                }
                want -= seg;
            }
            double r = 2.4 * Dpi() / 96.0, strength = std::sin(g_pulse * 3.14159265);
            Rgb glow = g_light ? Rgb{ 30, 150, 255 } : Rgb{ 205, 240, 255 };
            for (int yy = (int)(cy - r) - 1; yy <= (int)(cy + r) + 1; yy++)
                for (int xx = (int)(cx - r) - 1; xx <= (int)(cx + r) + 1; xx++) {
                    double d = std::hypot(xx + .5 - cx, yy + .5 - cy) / r;
                    if (d < 1) cv.Blend(xs[i] + xx, ys[i] + yy, glow, (int)(255 * strength * std::min(1.0, (1 - d) * 1.8)));
                }
        }
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void Paint() {
    if (!g_wnd) return;
    PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
    g_lastW = -1;  // force a re-position with the new size
}

void PaintMirror(Mirror& m) {
    g_dpiBar = m.bar;
    m.dpi = Dpi();
    PaintStrip(m.wnd, m.bar, m.hover, m.size);
    g_dpiBar = nullptr;
    m.lastLeft = INT_MIN;  // force a re-position with the new size
}

void HideMirror(Mirror& m) {
    if (m.lastLeft != INT_MIN || IsWindowVisible(m.wnd)) ShowWindow(m.wnd, SW_HIDE);
    m.lastLeft = INT_MIN;
}

// Places a mirror left of its taskbar's clock (and left of the Widgets button if present), or hides it
// when that spot is unknown or taken by task buttons: never guess and cover something.
void PositionMirror(Mirror& m) {
    RECT bar{};
    if (!GetWindowRect(m.bar, &bar) || (GetWindowLongW(m.bar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL)) { HideMirror(m); return; }
    g_dpiBar = m.bar;
    if (Dpi() != m.dpi || m.size.cy != bar.bottom - bar.top) PaintMirror(m), g_dpiBar = m.bar;
    int right = 0, minLeft = bar.left;
    bool known = false;
    Span span;
    // StartAllBack can put a full notification area on each secondary taskbar.
    // Use its left edge, as on the primary taskbar, to clear all of the tray icons.
    HWND tray = FindWindowExW(m.bar, nullptr, L"TrayNotifyWnd", nullptr);
    RECT tr{};
    if (tray && IsWindowVisible(tray) && GetWindowRect(tray, &tr) &&
        tr.left > bar.left && tr.right <= bar.right && tr.right > tr.left &&
        tr.bottom > bar.top && tr.top < bar.bottom) {
        right = tr.left;
        known = true;
    } else if (!IsXamlTaskbar(m.bar)) {  // Windows 10: the clock is a real child window (absent when it is turned off)
        HWND clock = FindWindowExW(m.bar, nullptr, L"ClockButton", nullptr);
        RECT cr{};
        right = clock && IsWindowVisible(clock) && GetWindowRect(clock, &cr) ? cr.left : bar.right - Px(8);
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(m.bar, nullptr, cls, nullptr);
            RECT fr{};
            if (feeds && IsWindowVisible(feeds) && GetWindowRect(feeds, &fr) && fr.left < right && fr.right > fr.left)
                right = fr.left;
        }
        known = true;
    } else {
        AcquireSRWLockShared(&g_wbLock);
        for (const auto& s : g_spans)
            if (s.bar == m.bar && s.measured) {
                span = s;
                right = s.hasClock ? s.clockLeft : bar.right - Px(8);
                if (s.hasWidgets && s.widgets.left < right && s.widgets.right > right - m.size.cx - Px(2))
                    right = s.widgets.left;
                if (s.hasTasks) minLeft = s.tasksRight + Px(4);
                known = true;
            }
        ReleaseSRWLockShared(&g_wbLock);
    }
    DWORD offset = 0, sz = sizeof(offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"SecondaryTaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset, &sz) ==
        ERROR_SUCCESS)
        right -= Px((int)offset);
    int left = right - m.size.cx - Px(2);
    int from = 0, to = 0;
    if (g_left && LeftRoom(span, bar, span.widgetsRight, Px(4), from, to) && to - from >= m.size.cx) {
        left = from;  // left end of this taskbar
        minLeft = bar.left;
    }
    g_dpiBar = nullptr;
    if (!known || m.size.cx <= 0 || left < minLeft) { HideMirror(m); return; }
    POINT p{ left, bar.top };
    ScreenToClient(m.bar, &p);
    bool covered = GetWindow(m.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && p.x == m.lastLeft) return;
    m.lastLeft = p.x;
    SetWindowPos(m.wnd, HWND_TOP, p.x, 0, m.size.cx, m.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void AddTool(HWND w) {
    if (!g_tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)w;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"BatteryHub" : g_tipText.c_str());
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void DestroyMirror(Mirror& m) {
    if (g_tip) {
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)m.wnd;
        SendMessageW(g_tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
    }
    if (m.wnd && IsWindow(m.wnd)) DestroyWindow(m.wnd);
    m.wnd = nullptr;
}

void SetMeasureMirrors(bool on) {
    AcquireSRWLockExclusive(&g_wbLock);
    bool changed = g_wbMirrors != on;
    g_wbMirrors = on;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (changed && on && g_wbWake) SetEvent(g_wbWake);  // measure the new taskbar now, not in 5 s
}

std::vector<HWND> SecondaryTaskbars() {
    std::vector<HWND> out;
    for (HWND t = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); t;
         t = FindWindowExW(nullptr, t, L"Shell_SecondaryTrayWnd", nullptr)) {
        RECT r{};
        if (IsWindowVisible(t) && GetWindowRect(t, &r) && r.right - r.left > r.bottom - r.top) out.push_back(t);  // horizontal only
    }
    return out;
}

// Keeps one mirror per secondary taskbar: adds them for new monitors, removes them for gone ones.
// Called from Reposition() about once a second; with one monitor it is a single FindWindowEx call.
// A monitor's lasting identity: its device interface name, which survives reboots, Explorer restarts and
// Windows renumbering the displays (falls back to "\\.\DISPLAYn").
std::wstring MonitorId(HWND bar) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONULL);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return L"";
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, EDD_GET_DEVICE_INTERFACE_NAME) && dd.DeviceID[0]) return dd.DeviceID;
    return mi.szDevice;
}

// "Display 3 (right)": the number Windows gives the display and where it sits next to the main one.
std::wstring MonitorLabel(HWND bar) {
    MONITORINFOEXW mi{}, pm{};
    mi.cbSize = pm.cbSize = sizeof(MONITORINFOEXW);
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(mon, &mi)) return T(L"Other monitor");
    std::wstring dev = mi.szDevice;
    size_t d = dev.find(L"DISPLAY");
    std::wstring label = d != std::wstring::npos ? TF(L"Display {number}", { { L"number", dev.substr(d + 7) } }) : T(L"Other monitor");
    POINT origin{ 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &pm)) {
        int dx = (mi.rcMonitor.left + mi.rcMonitor.right - pm.rcMonitor.left - pm.rcMonitor.right) / 2;
        int dy = (mi.rcMonitor.top + mi.rcMonitor.bottom - pm.rcMonitor.top - pm.rcMonitor.bottom) / 2;
        label += std::wstring(L" (") + T(abs(dx) >= abs(dy) ? (dx < 0 ? L"left" : L"right") : (dy < 0 ? L"above" : L"below")) + L")";
    }
    return label;
}

// Hides or shows the strip on the main taskbar (it keeps running, just out of sight).
void SetPrimaryHidden(bool hide) {
    if (hide == g_primaryHidden || !g_wnd) return;
    g_primaryHidden = hide;
    if (hide) {
        ShowWindow(g_wnd, SW_HIDE);
        if (g_rebar && IsWindow(g_rebar)) {  // Windows 10: give the task buttons their full width back
            RECT r;
            GetClientRect(g_taskbar, &r);
            PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
        }
    }
    g_lastW = -1;  // re-place (and show) on the next pass
}

void SyncMirrors() {
    std::vector<HWND> all = g_wnd ? SecondaryTaskbars() : std::vector<HWND>{}, bars;
    bool chosenFound = false;
    if (g_choice == L"all") {
        bars = all;
    } else if (g_choice != L"main") {
        for (HWND b : all)
            if (MonitorId(b) == g_choice) {
                bars.push_back(b);
                chosenFound = true;
                break;
            }
    }
    SetPrimaryHidden(chosenFound);
    for (auto it = g_mirrors.begin(); it != g_mirrors.end();) {
        if (std::find(bars.begin(), bars.end(), it->bar) == bars.end() || !IsWindow(it->wnd)) {
            DestroyMirror(*it);
            it = g_mirrors.erase(it);
        } else {
            ++it;
        }
    }
    for (HWND b : bars) {
        if (std::any_of(g_mirrors.begin(), g_mirrors.end(), [b](const Mirror& m) { return m.bar == b; })) continue;
        Mirror m;
        m.bar = b;
        m.wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, b, nullptr,
                                g_inst, nullptr);
        if (!m.wnd) continue;
        AddTool(m.wnd);
        g_mirrors.push_back(m);
        PaintMirror(g_mirrors.back());
    }
    SetMeasureMirrors(!g_mirrors.empty());
    for (auto& m : g_mirrors) PositionMirror(m);
}

void PaintAll() {
    Paint();
    for (auto& m : g_mirrors) PaintMirror(m);
}

// One frame of the pulse: the strips keep their size, so nothing has to be placed again.
void PaintPulseFrame() {
    if (!g_wnd) return;
    if (!g_primaryHidden) PaintStrip(g_wnd, g_taskbar, g_hover, g_size);
    for (auto& m : g_mirrors) {
        if (m.lastLeft == INT_MIN) continue;  // hidden
        g_dpiBar = m.bar;
        PaintStrip(m.wnd, m.bar, m.hover, m.size);
        g_dpiBar = nullptr;
    }
}

bool WantPulse() {
    bool charging = false;
    for (const auto& it : g_items) charging = charging || (it.charging && !it.full && !it.dim);
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return charging && animations;
}

// Starts the pulse timer when a shown device charges and stops it when none does.
void SyncPulse() {
    bool want = g_wnd && WantPulse();
    if (want == g_pulseOn) return;
    g_pulseOn = want;
    if (want) {
        SetTimer(g_wnd, kPulseIdle, kPulseEveryMs - kPulseMs, nullptr);
        return;
    }
    if (g_wnd) {
        KillTimer(g_wnd, kPulseIdle);
        KillTimer(g_wnd, kPulseFrame);
    }
    if (g_pulse >= 0) {
        g_pulse = -1;
        PaintPulseFrame();
    }
}

void OnPulseTimer(UINT_PTR id) {
    if (!WantPulse()) { SyncPulse(); return; }
    if (id == kPulseIdle) {
        KillTimer(g_wnd, kPulseIdle);
        g_pulseStart = GetTickCount64();
        g_pulse = 0;
        SetTimer(g_wnd, kPulseFrame, kPulseFrameMs, nullptr);
        return;
    }
    double t = (double)(GetTickCount64() - g_pulseStart) / kPulseMs;
    if (t >= 1) {
        g_pulse = -1;
        KillTimer(g_wnd, kPulseFrame);
        SetTimer(g_wnd, kPulseIdle, kPulseEveryMs - kPulseMs, nullptr);
    } else {
        g_pulse = t;
    }
    PaintPulseFrame();
}

// Hover highlight for the strip under the mouse: the primary one or a mirror.
void SetHover(HWND h, bool on) {
    if (h == g_wnd) {
        if (g_hover == on) return;
        g_hover = on;
        Paint();
        widget::Reposition();
        return;
    }
    for (auto& m : g_mirrors)
        if (m.wnd == h && m.hover != on) {
            m.hover = on;
            PaintMirror(m);
            PositionMirror(m);
        }
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            SetHover(h, true);
            return 0;
        }
        case WM_MOUSELEAVE:
            SetHover(h, false);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, msg == WM_RBUTTONUP, reinterpret_cast<LPARAM>(h));
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (h == g_wnd && (wp == kPulseIdle || wp == kPulseFrame)) {
                OnPulseTimer(wp);
                return 0;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    g_vertical = tb.bottom - tb.top > tb.right - tb.left;  // left / right taskbar: devices stacked in a column
    // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
    // and have to be narrowed so they never slide under the widget.
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    g_inst = inst;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = (UINT_PTR)g_wnd;
        ti.lpszText = const_cast<wchar_t*>(L"BatteryHub");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
    }
    WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    if (!g_primaryHidden) ShowWindow(g_wnd, SW_SHOWNOACTIVATE);  // hidden when only another monitor is chosen
    SyncPulse();
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        // Let Explorer lay the task buttons out again over the full width.
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    for (auto& m : g_mirrors) DestroyMirror(m);
    g_mirrors.clear();
    SetMeasureMirrors(false);
    g_primaryHidden = false;  // the next Create() decides again
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }  // its pulse timers go with it
    g_pulseOn = false;
    g_pulse = -1;
    g_taskbar = nullptr;
    g_hover = false;
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    if (tooltip != g_tipText && g_tip) {
        g_tipText = tooltip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        ti.uId = (UINT_PTR)g_wnd;
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        for (const auto& m : g_mirrors) {
            ti.uId = (UINT_PTR)m.wnd;
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    PaintAll();
    Reposition();
    SyncPulse();
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    PaintAll();
}

void SetBackground(bool on) {
    g_background = on;
    PaintAll();
}

// The stacked and charge-ring styles are gone: the strip has one style (isometric icons). These remain
// only so that callers still compile, and do nothing.
void SetStacked(bool) {}
void SetRingStyle(bool) {}

void SetLeftSide(bool on) {
    g_left = on;
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbLeft = on;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (g_wbWake) SetEvent(g_wbWake);  // measure the Start button now rather than in a few seconds
    if (g_wnd) Reposition();
}

void SetMonitorChoice(const std::wstring& choice) {
    g_choice = choice.empty() ? L"all" : choice;
    SyncMirrors();  // adds, removes or hides strips right away
    if (g_wnd) Reposition();
}

std::vector<MonitorOption> OtherMonitors() {
    std::vector<MonitorOption> out;
    for (HWND b : SecondaryTaskbars()) out.push_back({ MonitorId(b), MonitorLabel(b) });
    std::sort(out.begin(), out.end(), [](const MonitorOption& a, const MonitorOption& b) { return a.label < b.label; });
    return out;
}

bool Reposition() {
    if (!Alive()) return false;
    SyncMirrors();
    if (g_primaryHidden) return true;  // only another monitor's taskbar shows the strip
    if (Dpi() != g_lastDpi) { g_lastDpi = Dpi(); Paint(); }
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr, bar;
    GetWindowRect(tray, &tr);
    GetWindowRect(g_taskbar, &bar);
    // The taskbar can be moved between a horizontal and a vertical edge while the app runs.
    bool vertical = bar.bottom - bar.top > bar.right - bar.left;
    if (vertical != g_vertical) {
        g_vertical = vertical;
        Paint();
    }
    if (g_vertical) {
        // Left or right taskbar (PR #321): the column sits just above the notification area, centred in
        // the bar's width.
        RECT client{};
        GetClientRect(g_taskbar, &client);
        POINT anchor{ bar.left, tr.top };
        ScreenToClient(g_taskbar, &anchor);
        int top = anchor.y - g_size.cy - Px(2);
        DWORD offset = 0, osz = sizeof(offset);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr, &offset,
                         &osz) == ERROR_SUCCESS)
            top -= Px((int)offset);
        top = std::clamp(top, 0, (int)std::max<LONG>(0, client.bottom - g_size.cy));
        int left = (int)std::max<LONG>(0, (client.right - g_size.cx) / 2);
        // Windows 10: shorten the task-button area when it would reach behind the column.
        if (g_rebar && IsWindow(g_rebar)) {
            RECT rr{};
            GetWindowRect(g_rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(g_taskbar, &tl);
            if (tl.y < top && tl.y + (rr.bottom - rr.top) > top)
                MoveWindow(g_rebar, tl.x, tl.y, rr.right - rr.left, top - tl.y, TRUE);
        }
        bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
        if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && g_lastW == g_size.cy && g_lastLeft == top)
            return true;
        g_lastTray = tr;
        g_lastBar = bar;
        g_lastW = g_size.cy;
        g_lastLeft = top;
        SetWindowPos(g_wnd, HWND_TOP, left, top, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        return true;
    }
    // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
    // edge, so a window's client-space left edge is its screen-space right edge.
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    RECT wb = WidgetsButtonRect();
    int left = 0, from = 0, to = 0;
    bool onLeft = false;
    if (g_left && !rtl) {
        AcquireSRWLockShared(&g_wbLock);
        Span s = g_primarySpan.bar == g_taskbar ? g_primarySpan : Span{};
        ReleaseSRWLockShared(&g_wbLock);
        onLeft = LeftRoom(s, bar, wb.right > wb.left ? (int)wb.right : 0, Px(4), from, to) && to - from >= g_size.cx;
    }
    if (onLeft) {
        POINT p{ from, bar.top };
        ScreenToClient(g_taskbar, &p);
        left = p.x;
    } else {
    int edge = rtl ? tr.right : tr.left;  // screen x we sit next to
    // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
    // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
    // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
    // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
    // width, so the widget follows it.
    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    // Step past the Widgets button when it sits where we would go.
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    left = p.x - g_size.cx - Px(2);


    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset",
                     RRF_RT_REG_DWORD, nullptr, &customOffset, &sz) == ERROR_SUCCESS) {
        left -= Px((int)customOffset);
    }

    // Windows 10: Explorer re-expands the task buttons on its own layout passes; narrow them again.
    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }
    }  // right side

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && EqualRect(&wb, &g_lastWidgets) &&
        g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace widget