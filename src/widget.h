#pragma once
// A small transparent strip on the taskbar, just left of the notification area, showing
// "[icon] 30%  [icon] 93%" with a small isometric icon for each device (widget_icons.h). Implemented as a layered child window of the taskbar (Shell_TrayWnd), with a
// copy on each secondary-monitor taskbar (Shell_SecondaryTrayWnd) next to its clock.
#include <windows.h>
#include <string>
#include <vector>
#include "brands.h"
#include "devices.h"

struct WidgetItem {
    DeviceKind kind = DeviceKind::Other;
    int  percent  = -1;     // -1 = unknown
    std::wstring text;      // shown instead of the percentage when set (coarse levels like "Low")
    bool charging = false;  // the icon is plugged into a cable, with a pulse of light now and then
    bool full     = false;  // on power and fully charged: cable with a green plug, and a green check mark
    bool dim      = false;  // asleep / stale value
    bool low      = false;
    bool bluetooth = false;  // not used by the strip any more (unknown device types share one icon)
    std::wstring label;      // tiny caption on the icon; set only when two devices of one kind are shown
    Accent accent  = Accent::None;  // not used by the strip any more (brand colours were removed); kept for callers
};

namespace widget {
// Creates the widget on the primary taskbar. Clicks are reported by posting `clickMsg` to `notifyWnd`.
bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme);
void Destroy();
bool Alive();
void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip);
void SetLightTheme(bool light);
// Draws a rounded plate in the taskbar's colours behind the text (kept across Create/Destroy).
void SetBackground(bool on);
// No-ops: the stacked and charge-ring styles were removed, the strip has one style now. Kept so that
// existing callers compile.
void SetStacked(bool on);
void SetRingStyle(bool on);
// Left end of the taskbar (between its edge or the Widgets button and the Start button) instead of next
// to the tray. Windows 11 with centred taskbar icons only; elsewhere, or without room, it stays right.
void SetLeftSide(bool on);
// Which taskbars show the strip: L"all" (default), L"main", or the id of one other monitor from
// OtherMonitors() (then only that monitor shows it). Kept across Create/Destroy.
void SetMonitorChoice(const std::wstring& choice);
struct MonitorOption {
    std::wstring id;     // lasting monitor identity, stored in the settings
    std::wstring label;  // "Display 3 (right)"
};
// The monitors other than the main one that have a horizontal taskbar (empty with one monitor).
std::vector<MonitorOption> OtherMonitors();
// Re-checks the taskbar layout and moves the widget if needed; call about once a second.
// Returns false when the taskbar it was attached to is gone.
bool Reposition();
}
