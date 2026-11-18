// Logitech G435 on its original Realtek LIGHTSPEED receiver (046D:0ACB). The receiver speaks no
// HID++ and has no passive status channel; the battery is read through the Realtek "PA" command
// bridge on feature report 6 of the vendor collection (usage page FFC0, 64-byte feature report):
//   [06 01 03]                    enter bootloader mode (mutes USB audio while open)
//   [06 04 03 00 len] + PA frame  send a PA command
//   [06 08 03]                    exit bootloader mode
//   PA frames: 50 41 0E 00 02 E1 01  remote mode (forward to the headset)
//              50 41 06 00 01 00 31  OTA get battery -> reply "06 80 00 <mV u16 LE>"
//              50 41 0E 00 02 E1 00  back to local mode
// Protocol from github.com/fr0styJS/g435-lightspeed-battery (MIT), ported byte for byte.
//
// Every read cuts the headset audio for about a second, so the headset is read at most every
// 30 minutes (or when "Refresh now" is clicked) and the last value is reported in between.
// While the headset is off or unlinked, it is retried every 5 minutes.
//
// The headset reports no charging state (the GRS status byte and ATB 0 read the same on and off
// the charger). Plugging the charger in raises the cell voltage by about 180-250 mV within
// seconds, while normal use moves it by tens of mV between reads. So a rise of 100 mV between two
// reads sets an estimated charging flag. Unplugging a half-charged headset drops it by about as
// much, but near full the charger holds the cell at about 4.2 V with little current, so unplugging
// it then drops only 50-80 mV. Charging never lowers the voltage, so any fall of 40 mV clears the
// flag, and it also expires 3 hours after it was set (a full charge takes less). The last voltage,
// flag and start time are kept in the registry, so the comparison also works across restarts.
#include "drivers.h"

#include <atomic>
#include <ctime>

namespace {

constexpr USHORT kVid = 0x046D, kPid = 0x0ACB;
constexpr BYTE kReport = 6;
constexpr ULONGLONG kReadEveryMs = 30ull * 60 * 1000;
constexpr ULONGLONG kRetryEveryMs = 5ull * 60 * 1000;

std::atomic<bool> g_forceRead{ false };
bool g_haveReading = false;     // last attempt produced a voltage
bool g_attempted = false;       // at least one attempt was made in this process
int g_lastMv = 0;
ULONGLONG g_lastAttempt = 0;
bool g_charging = false;        // estimated from voltage jumps
bool g_loaded = false;          // saved state read from the registry

constexpr int kJumpMv = 100;    // a rise this big between two reads means plugged in
constexpr int kDropMv = 40;     // a fall this big means unplugged (readings wobble by about 20 mV)
constexpr long long kChargeMaxSec = 3ll * 60 * 60;
constexpr int kChargeOffsetMv = 180;
const wchar_t* kKey = L"Software\\WirelessBattery";

DWORD LoadDword(const wchar_t* name) {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}

void SaveDword(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v)); }

// Updates the charging estimate with a new reading and remembers it for the next comparison.
void TrackCharging(int mv) {
    static DWORD since = 0;  // when the charging flag was set (Unix seconds)
    if (!g_loaded) {
        g_loaded = true;
        g_lastMv = (int)LoadDword(L"G435LastMv");
        g_charging = LoadDword(L"G435Charging") != 0;
        since = LoadDword(L"G435ChargeSince");
    }
    DWORD now = (DWORD)time(nullptr);
    if (g_lastMv) {
        if (mv - g_lastMv >= kJumpMv) {
            if (!g_charging) since = now;
            g_charging = true;
        } else if (g_lastMv - mv >= kDropMv) {
            g_charging = false;
        }
    }
    if (g_charging && (long long)now - since >= kChargeMaxSec) g_charging = false;  // also no start time saved
    g_lastMv = mv;
    SaveDword(L"G435LastMv", (DWORD)mv);
    SaveDword(L"G435Charging", g_charging ? 1 : 0);
    SaveDword(L"G435ChargeSince", g_charging ? since : 0);
}

// Generic 3.7 V Li-ion curve: Logitech publishes none for the G435, so the percentage is an
// estimate (the millivolts are exact).
int G435Percent(int mv) {
    static const int v[] = { 4200, 4100, 4000, 3900, 3800, 3750, 3700, 3650, 3600, 3500, 3300 };
    static const int p[] = { 100, 90, 80, 65, 50, 40, 30, 20, 10, 5, 0 };
    return Interpolate(mv, v, p, 11);
}

bool Send(HidDev& d, std::initializer_list<BYTE> frame) {
    std::vector<BYTE> b = { kReport, 4, 3, 0, (BYTE)frame.size() };
    b.insert(b.end(), frame.begin(), frame.end());
    return d.SetFeature(b);
}

// One battery read. Returns the headset voltage in mV, or 0 when the headset did not answer.
int ReadMillivolts(HidDev& d) {
    std::vector<BYTE> r;
    int mv = 0;
    if (!d.SetFeature({ kReport, 1, 3, 0, 0, 0, 0 })) return 0;  // enter bootloader
    Sleep(250);
    d.GetFeature(kReport, r);
    if (Send(d, { 0x50, 0x41, 0x0E, 0x00, 0x02, 0xE1, 0x01 })) {  // remote mode
        Sleep(150);
        if (Send(d, { 0x50, 0x41, 0x06, 0x00, 0x01, 0x00, 0x31 })) {  // get battery
            ULONGLONG end = GetTickCount64() + 800;
            while (!mv && GetTickCount64() < end) {
                if (d.GetFeature(kReport, r)) {
                    for (size_t i = 1; i + 4 < r.size(); i++) {
                        if (r[i] == 0x06 && r[i + 1] == 0x80 && r[i + 2] == 0x00) {
                            mv = r[i + 3] | (r[i + 4] << 8);
                            break;
                        }
                    }
                }
                if (!mv) Sleep(30);
            }
        }
        Send(d, { 0x50, 0x41, 0x0E, 0x00, 0x02, 0xE1, 0x00 });  // local mode
        Sleep(50);
    }
    d.SetFeature({ kReport, 8, 3, 0, 0, 0, 0 });  // exit bootloader, always
    Sleep(100);
    // In local mode the receiver answers with its own value (~102); a real cell reads 3.0-4.5 V.
    return (mv >= 3000 && mv <= 4500) ? mv : 0;
}

}  // namespace

void G435RequestRead() { g_forceRead = true; }

void PollG435(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kPid)) return;
    auto colls = ctx.Find(kVid, kPid, [](const HidColl& c) { return c.page() == 0xFFC0 && c.featLen() == 64; });
    if (colls.empty()) {  // receiver unplugged: forget the reading
        g_haveReading = g_attempted = false;
        return;
    }
    ctx.Claim(kVid, kPid);  // keep the generic fallbacks off this receiver

    ULONGLONG now = GetTickCount64();
    ULONGLONG wait = g_haveReading ? kReadEveryMs : kRetryEveryMs;
    if (g_forceRead.exchange(false) || !g_attempted || now - g_lastAttempt >= wait) {
        HidDev d;
        if (d.Open(*colls[0])) {
            int mv = ReadMillivolts(d);
            g_attempted = true;
            g_lastAttempt = now;
            g_haveReading = mv != 0;
            if (mv) TrackCharging(mv);
        }
    }
    if (!g_haveReading) return;  // headset off or not linked: hide it

    DeviceStatus s;
    s.key = L"g435:" + Hex4(kPid);
    s.name = L"Logitech G435";
    s.via = L"LIGHTSPEED receiver";
    s.kind = DeviceKind::Headset;
    s.millivolts = g_lastMv;
    // On the charger the cell reads about 180 mV above its resting voltage (measured: 4152 mV
    // charging, 3972 mV ten seconds after unplugging), so that offset is taken off the estimate.
    // It also keeps the voltage from passing for "fully charged".
    s.percent = G435Percent(g_charging ? g_lastMv - kChargeOffsetMv : g_lastMv);
    s.charging = g_charging;
    if (g_charging) s.detail = L"charging guessed from voltage";
    ctx.out.push_back(s);
}
