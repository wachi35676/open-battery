#pragma once
// Extra sections for `--dump`: HID devices that could not be opened, every USB device (including ones
// without a HID interface, e.g. audio-only or WinUSB dongles and serial ports) and the Bluetooth devices
// Windows knows about.
#include <string>

namespace diagnostics {
std::wstring DumpExtras();
}
