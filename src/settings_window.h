#pragma once
// The settings window, in the layout of the Windows 11 Settings app: Mica, a navigation list on the left,
// pages of setting cards (toggle switches, combo boxes, buttons) and one expandable card per device.
// Every setting the app has lives here; changes apply at once.
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include "devices.h"

namespace settings_window {

struct Choice {
    std::vector<std::wstring> options;
    int selected = 0;
};

struct Device {
    std::wstring key, name, detail;  // detail: "LIGHTSPEED receiver · 92%"
    DeviceKind kind = DeviceKind::Other;
    bool bluetooth = false;
    int percent = -1;
    bool charging = false;
    bool shown = true;      // on the taskbar / in the tray
    bool renamed = false;
    bool first = false, last = false;  // in the left-to-right order
    Choice alert;           // low battery notification threshold
};

struct State {
    bool light = false;
    // Taskbar
    Choice showAs, style, position, monitors;
    bool background = false, brandColors = false, dimSleeping = true, hideAsleep = false, showDisconnected = false;
    bool widget = true;  // false in tray mode: the widget-only settings are disabled
    // Devices
    std::vector<Device> devices;
    // General
    // Desktop widget
    bool desktop = false, desktopOnTop = false, desktopLocked = false;
    Choice desktopSize, desktopBackground;
    bool autostart = false, recordHistory = true;
    Choice poll, language, panel;
    std::wstring version;
};

enum class Page { Taskbar = 0, Devices, Desktop, General, About };

enum class Action {
    ShowAs, Style, Position, Monitor, Background, BrandColors, DimSleeping, HideAsleep, ShowDisconnected,
    Desktop, DesktopSize, DesktopOnTop, DesktopLocked, DesktopBackground, DesktopReset,
    Autostart, RecordHistory, Poll, Language, AddLanguage, Panel, Refresh, Exit, OpenProject,
    DeviceShown, DeviceLeft, DeviceRight, DeviceRename, DeviceResetName, DeviceAlert, DeviceHistory
};

// `value` is the chosen option index (or 0/1 for switches); `key` the device for Device* actions.
using Handler = std::function<void(Action action, int value, const std::wstring& key)>;
using Provider = std::function<State()>;

void Show(HINSTANCE inst, Page page, Provider provider, Handler handler);
void Refresh();  // settings or readings changed elsewhere; theme changes too
bool Visible();
void Close();

}  // namespace settings_window
