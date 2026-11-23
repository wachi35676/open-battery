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
enum class Command { Refresh, Settings, History };
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
}
