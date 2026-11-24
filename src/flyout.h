#pragma once
// The panel that opens when the taskbar strip (or a tray icon) is left-clicked: an isometric desk with the
// connected devices on it (headset on its stand, mouse, keyboard, phone, controller), each with a leader
// line to its battery level, any further devices in a short list, and a Windows 11 footer with refresh
// and settings. Drawn with Direct2D on the Mica / Acrylic / Clear glass backdrop; follows light and dark.
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "devices.h"
#include "fluent.h"

namespace flyout {
struct Device {
    std::wstring key, name, state;  // state: "LIGHTSPEED receiver", "Charging", "Asleep, last known level"...
    DeviceKind kind = DeviceKind::Other;
    bool bluetooth = false;
    int percent = -1;                // -1: unknown
    std::wstring levelText;          // shown instead of the number for coarse levels
    bool charging = false, full = false, dim = false, low = false;
};
struct Labels {
    std::wstring summary, emptyTitle, emptyBody;
    std::wstring headset, mouse, keyboard, phone, gamepad;  // names under the levels on the desk
};
struct Model {
    std::vector<Device> devices;  // in the user's order
    Labels text;
    bool light = false;           // follows the taskbar theme
    fluent::Backdrop backdrop = fluent::Backdrop::Mica;
};
enum class Command { Refresh, Settings, History, DesktopSettings };
// `key` is the clicked device (Command::History).
using Handler = std::function<void(Command cmd, const std::wstring& key)>;

// Shows the panel next to `anchor` (the clicked strip or the cursor), on that monitor.
// Returns false where the backdrop is not available (before Windows 11 22H2).
bool Show(HINSTANCE inst, const RECT& anchor, const Model& model, Handler handler);
void Update(const Model& model);  // new readings or settings while it is open
void Hide();
bool Visible();
// True for a short moment after the panel closed because it lost focus: a click on the strip that
// caused it should not reopen it.
bool JustClosed();

// ---- the desktop widget -----------------------------------------------------------------------
// The same panel as a window that stays on the desktop: drag it anywhere, pick one of a few fixed sizes.
// It never takes focus, so Mica cannot be used (Windows draws it flat grey for a window that is not
// active); model.backdrop should be Glass, Frosted or Solid. A right click sends Command::DesktopSettings.
constexpr int kWidgetSizes = 5;  // Small, Medium, Large, Extra large, Huge: 75%, 100%, 125%, 150%, 200%
struct WidgetOptions {
    bool hasPosition = false;  // false: put it on the second monitor (or in the corner of the only one)
    int x = 0, y = 0;          // screen position of its top-left corner
    int size = 1;              // 0 .. kWidgetSizes - 1
    bool onTop = false;        // above other windows; otherwise it is an ordinary window that stays where it is
    bool locked = false;       // cannot be dragged
    std::function<void(int x, int y)> moved;  // after the user dropped it
};
bool ShowWidget(HINSTANCE inst, const Model& model, Handler handler, const WidgetOptions& options);
void UpdateWidget(const Model& model, const WidgetOptions& options);  // readings, theme, size, position
void HideWidget();
bool WidgetVisible();
}
