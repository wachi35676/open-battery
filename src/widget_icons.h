#pragma once
// Device icons for the taskbar strip. They are Windows' own (the ones in Devices and Printers), loaded at
// run time from System32\DDORes.dll at the exact pixel size shown. A device on power gets a small cable
// drawn into its port. When Windows does not have those icons, and for devices of no known kind, small
// isometric icons drawn here in software are used: the same solid, lit look as the desk in the click
// panel (flyout.cpp), designed for about 24 pixels. Everything is cached, so repainting stays cheap.
#include <windows.h>
#include <vector>
#include "devices.h"

namespace widget_icons {

// Charging and Full show the device plugged in: a short cable with its plug in blue (charging) or green (full).
enum class State { Normal, Charging, Full, Low };

struct Sprite {
    int w = 0, h = 0;
    std::vector<DWORD> px;  // premultiplied BGRA, top-down
    int bodyY = 0, bodyH = 0;  // the device itself, without its cable: centre this in the taskbar
    struct Pt { float x, y; };
    std::vector<Pt> cable;     // Charging / Full: the cable's centre line in sprite pixels, ending at the device
};

// The icon for `kind`, about `size` pixels high (a system icon comes in the nearest size it was drawn at).
// `light` is the taskbar theme. The result is cached per (kind, state, theme, size) and stays valid
// for the life of the process.
const Sprite& Get(DeviceKind kind, State state, bool light, int size);

}  // namespace widget_icons
