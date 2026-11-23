# About this build

A personal continuation of krisgfx9/Wireless-Device-Battery-Monitor (BatteryHub). The original GitHub
account was suspended around 21 November 2026 and the repository is offline. This build starts from the last
upstream C++ commit, `632f204` (21 November 2026, the maintainer's final PR merges), recovered from a public
mirror (ferloom/Wireless-Device-Battery-Monitor, before its Rust rewrite) and cross-checked against a second,
unrelated copy whose base commit `fd2267d` is in the same history. Our last directly fetched upstream commit,
`285d642`, is an ancestor of it.

Upstream merged two of our pull requests before it went offline: Phone Link charging (#1254) and the green
check mark for fully charged devices (#1383).

## Added here

| Change | Notes |
|---|---|
| Logitech G435 on its older Realtek receiver (`046D:0ACB`); longer wait for sleeping Logitech receiver mice | was PR #93, never merged |
| G435: charging estimated from a 100 mV jump between reads under 40 minutes apart; cleared once the voltage sinks 25 mV below its charging peak, or after 3 hours; shown as full when the voltage stops rising for 45 minutes near 4.1 V | the headset reports no charging state |
| G435: a jammed receiver (only FF replies) keeps the headset visible, dimmed, with a replug hint; first read waits 30 s after a replug or boot; reads serialised across processes | a read right after plugging in jams the receiver |
| *On the left (Windows 11 with centred icons)*: the widget between the taskbar's left edge (or the Widgets button) and the Start button | |
| *Charge rings instead of percentages*: a ring around each device icon shows the charge; exact values stay in the tooltip | from our old Customize menu; device order and left placement already exist |
| Visual Studio build (`build-msvc.cmd`), and a one-line MSVC compile fix in `razer_headset.cpp` | |

## Removed here

The update checker and installer-update path, the *Request support for a device* window (it posted to
krisgfx.com), *Support the project*, release signing, the Discord release announcement and the funding config.
The app makes no network connections. The `--dump` diagnostics that lived in the request window's code are
kept in `src/dump_extras.cpp`.

## Building

- **Visual Studio 2022+** (C++ desktop workload): run `build-msvc.cmd`. It generates `build/version.h` as a
  `dev` build. `msvc_compat.h` maps one MinGW-only WinRT type name for MSVC; it is only used by this script.
- **MinGW-w64** (MSYS2): `make` or `build.bat`, as in the README.

Then run `WirelessBattery.exe --list` to see what it finds, and `WirelessBattery.exe` for the taskbar widget.
The file name stays `WirelessBattery.exe` so existing installs keep their *Start with Windows* entry.

## Settings

Stored under `HKCU\Software\WirelessBattery` and changed from the right-click menu. This build adds `LeftSide`
(the left placement), `RingStyle` (charge rings) and `G435LastMv`, `G435LastAt`, `G435Charging`, `G435ChargeSince`, `G435ChargePeak` and
`G435ChargePeakAt` (the last G435 reading and the charging estimate).
