#include "drivers.h"

#include <map>
#include <memory>
#include <set>

namespace {

constexpr USHORT kMchoseVid = 0x3837;
constexpr USHORT kV9TurboPlusPid = 0x600A;
constexpr BYTE kBatteryReportId = 0x55;
constexpr BYTE kBatteryCommand = 0x65;
constexpr BYTE kStatusDischarge = 2;
constexpr BYTE kStatusCharging = 3;
constexpr BYTE kStatusFull = 4;
constexpr BYTE kStatusAsleep = 26;

struct Battery {
    int percent = -1;
    bool charging = false, full = false, asleep = false;
};

bool DecodeBattery(const std::vector<BYTE>& r, Battery& b) {
    if (r.size() < 4 || r[0] != kBatteryReportId || r[1] != kBatteryCommand) return false;
    BYTE status = r[3];
    if (status == kStatusAsleep) {
        b.asleep = true;
        return true;
    }
    if (status != kStatusDischarge && status != kStatusCharging && status != kStatusFull) return false;
    if (r[2] > 100) return false;
    b.percent = r[2];
    b.charging = status == kStatusCharging;
    b.full = status == kStatusFull;
    b.asleep = false;
    return true;
}

struct Session {
    std::unique_ptr<HidDev> dev;
    Battery last;
    bool seen = false;
};
std::map<std::wstring, Session> g_v9turbo;

bool DrainQueued(HidDev& d, Battery& b) {
    bool got = false;
    for (int i = 0; i < 64; i++) {
        std::vector<BYTE> r;
        if (d.Read(r, 30) < 0) break;
        if (DecodeBattery(r, b)) got = true;
    }
    return got;
}

bool WaitBattery(HidDev& d, Battery& b, ULONGLONG end) {
    do {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) return false;
        std::vector<BYTE> r;
        if (d.Read(r, left) < 0) return false;
        if (DecodeBattery(r, b)) return true;
    } while (GetTickCount64() < end);
    return false;
}

}  // namespace

void PollMchoseV9Turbo(PollContext& ctx) {
    if (ctx.IsClaimed(kMchoseVid, kV9TurboPlusPid)) return;
    std::set<std::wstring> present;
    for (const HidColl* c : ctx.Find(kMchoseVid, kV9TurboPlusPid, [](const HidColl& c) {
             return c.page() == 0xFF90 && c.usage() == 0x0001 && c.inLen() >= 64;
         })) {
        present.insert(c->path);
        Session& s = g_v9turbo[c->path];
        bool fresh = !s.dev;
        if (fresh) {
            s.dev = std::make_unique<HidDev>();
            if (!s.dev->Open(*c)) {
                g_v9turbo.erase(c->path);
                continue;
            }
        }
        bool got = DrainQueued(*s.dev, s.last);
        if (!got && fresh && !s.seen) got = WaitBattery(*s.dev, s.last, GetTickCount64() + 1000);
        if (!got && !s.seen) continue;
        s.seen = true;

        DeviceStatus st;
        st.key = L"mchose:v9turbo";
        st.name = HidProduct(*c);
        if (st.name.empty()) st.name = L"MCHOSE V9 Turbo+";
        st.kind = DeviceKind::Headset;
        st.via = L"2.4 GHz receiver";
        if (s.last.asleep) {
            st.asleep = true;
        } else {
            st.percent = s.last.percent;
            st.charging = s.last.charging;
            st.full = s.last.full;
        }
        st.graceSec = 1800;
        ctx.Add(st, kMchoseVid, kV9TurboPlusPid);
        break;
    }
    for (auto it = g_v9turbo.begin(); it != g_v9turbo.end();)
        it = present.count(it->first) ? std::next(it) : g_v9turbo.erase(it);
}
