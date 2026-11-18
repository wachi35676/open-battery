# About this fork

Fork of krisgfx9/Wireless-Device-Battery-Monitor (BatteryHub). The original GitHub account was suspended
around 21 November 2026 and the repository is offline. This branch is built on the last upstream C++ commit,
`51d5213` (21 November 2026), recovered from a public mirror (ferloom/Wireless-Device-Battery-Monitor, before
its Rust rewrite) and cross-checked against a second, unrelated copy whose base commit `fd2267d` is in the same
history. Our last directly fetched upstream commit, `285d642`, is an ancestor of it.

Upstream merged two of our pull requests before it went offline: Phone Link charging (#1254) and the green
check mark for fully charged devices (#1383). On top of upstream, this branch adds:

| Change | Status |
|---|---|
| Logitech G435 on its older Realtek receiver (`046D:0ACB`); longer wait for sleeping Logitech receiver mice | PR #93, not merged before upstream went offline |
| G435 (`046D:0ACB`): charging estimated from voltage jumps, since the headset reports no charging state; cleared by any 40 mV fall or after 3 hours | part of PR #93 |
| *On the left (Windows 11 with centred icons)*: the widget between the taskbar's left edge (or the Widgets button) and the Start button | fork only |
| Visual Studio build (`build-msvc.cmd`), and a one-line MSVC compile fix in `razer_headset.cpp` | fork only |

Upstream (the newer `046D:0ADF` dongle driver) already reads the G435 on that dongle; this branch only adds the
older `0ACB` receiver.

## Building

- **Visual Studio 2022+** (C++ desktop workload): run `build-msvc.cmd`. `msvc_compat.h` maps one MinGW-only
  WinRT type name for MSVC; it is only used by this script.
- **MinGW-w64** (MSYS2): `make` or `build.bat`, as in the upstream README.

Then run `WirelessBattery.exe --list` to see what it finds, and `WirelessBattery.exe` for the taskbar widget.

## Settings

Stored under `HKCU\Software\WirelessBattery` and changed from the right-click menu. This fork adds `LeftSide`
(the left placement) and `G435LastMv` / `G435Charging` / `G435ChargeSince` (the last G435 reading, for the
charging estimate).
