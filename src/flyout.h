#pragma once
// The panel that opens when the taskbar strip (or a tray icon) is left-clicked, in the style of the
// Windows 11 flyouts (Quick Settings, Bluetooth & devices): a card per device with its battery, quick
// toggle tiles, and a footer with refresh and settings. Drawn with Direct2D on the Mica (or Acrylic)
// system backdrop. Right-click keeps the full menu, which the settings button also opens.
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
struct Toggle {
    UINT id = 0;
    std::wstring label;
    wchar_t glyph = 0;
    bool on = false;
};
struct Labels {
    std::wstring summary, settings, refresh, emptyTitle, emptyBody, more, historyHint;
};
struct Model {
    std::vector<Device> devices;  // in the user's order
    std::vector<Toggle> toggles;  // three per row
    Labels text;
    bool light = false;           // follows the taskbar theme
    fluent::Backdrop backdrop = fluent::Backdrop::Mica;
};
enum class Command { Refresh, Settings, History, Toggle };
// `id` is the toggle's id (Command::Toggle); `key` the clicked device (Command::History).
using Handler = std::function<void(Command cmd, UINT id, const std::wstring& key)>;

// Shows the panel next to `anchor` (the clicked strip or the cursor), on that monitor.
// Returns false where the backdrop is not available (before Windows 11 22H2): use the menu then.
bool Show(HINSTANCE inst, const RECT& anchor, const Model& model, Handler handler);
void Update(const Model& model);  // new readings or settings while it is open
void Hide();
bool Visible();
// True for a short moment after the panel closed because it lost focus: a click on the strip that
// caused it should not reopen it.
bool JustClosed();
}
