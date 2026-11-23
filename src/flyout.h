#pragma once
// The panel that opens when the taskbar strip (or a tray icon) is left-clicked: the main device large,
// the others in a list, quick toggles and a few actions. Drawn with Direct2D on Windows 11's own
// Acrylic or Mica backdrop. Right-click keeps the full menu, which "All settings" also opens.
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "devices.h"

namespace flyout {
struct Device {
    std::wstring key, name, via, state;  // state: "Charging", "Asleep, last known level", ...
    DeviceKind kind = DeviceKind::Other;
    bool bluetooth = false;
    int percent = -1;                     // -1: unknown
    std::wstring levelText;               // shown instead of the number for coarse levels
    bool charging = false, full = false, dim = false, low = false;
};
struct Toggle {
    UINT id = 0;
    std::wstring label;
    wchar_t glyph = 0;
    bool on = false;
};
struct Labels {
    std::wstring greeting, summary, settings, refresh, history, exit, on, off, emptyTitle, emptyBody, more;
};
struct Model {
    std::vector<Device> devices;  // in the user's order; the first is shown large unless another was picked
    std::vector<Toggle> toggles;  // two per row
    Labels text;
    bool light = false;           // follows the taskbar theme
    bool mica = false;            // Mica instead of Acrylic
};
enum class Command { Refresh, Exit, Settings, History, Toggle };
// `id` is the toggle's id (Command::Toggle); `key` the device shown large (Command::History).
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
