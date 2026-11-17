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
#include "drivers.h"

#include <atomic>

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
            if (mv) g_lastMv = mv;
        }
    }
    if (!g_haveReading) return;  // headset off or not linked: hide it

    DeviceStatus s;
    s.key = L"g435:" + Hex4(kPid);
    s.name = L"Logitech G435";
    s.via = L"LIGHTSPEED receiver";
    s.kind = DeviceKind::Headset;
    s.millivolts = g_lastMv;
    s.percent = G435Percent(g_lastMv);
    ctx.out.push_back(s);
}
