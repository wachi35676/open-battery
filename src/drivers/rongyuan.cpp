// Rongyuan 2.4 GHz receivers, used by Attack Shark, Ajazz and many other mice and keyboards (and
// some MSI devices). One feature report returns both the keyboard and the mouse slot:
//   request [00 F7 00 ...] on interface 2, vendor collection FFFF:0002
//   reply   [00][ok][keyboard %][mouse %][keyboard offline][mouse offline]...
// Receiver list and protocol from github.com/qu1ncky/BatteryTray (RongyuanDongleProvider, MIT),
// itself taken from the official ATTACK SHARK web driver.
// The status report carries no model name, so names come from the receiver's product string with
// the link words stripped ("AJAZZ 2.4G 8K" -> "AJAZZ wireless mouse"). 3151:402D is shared by
// several mice (Attack Shark R3, AJAZZ AJ179 Pro, ...); it is only called "Attack Shark R3" when
// the product string names no other brand.
#include "drivers.h"

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace {

const UINT kReceivers[] = {
    0x0DB0197E, 0x0DB08E9A, 0x145F0376, 0x145F038D, 0x14A5C045, 0x25AA2008, 0x25AA4008, 0x25AA4018, 0x31514011,
    0x31514014, 0x31514017, 0x31514021, 0x31514027, 0x3151402D, 0x31514038, 0x31515006, 0x31515007, 0x31515008,
    0x31515020, 0x31515026, 0x3151502B, 0x31515031, 0x31515037, 0x31515038, 0x3151503D, 0x3151503F, 0x31515040,
    0x31515042, 0x31515045, 0x3151504B, 0x3151504E, 0x31515050, 0x31515052, 0x31515055, 0x3151505A, 0x342DE3E0,
    0x342DE3E1, 0x342DE3E2, 0x342DE3E6, 0x342DE3E7, 0x342DE3E8, 0x342DE3F5, 0x342DE3F6, 0x342DE3F7, 0x342DE3FB,
    0x342DE3FC, 0x342DE3FD, 0x347AA302, 0x347AA305, 0x374AA205, 0x374AA217, 0x374AA220, 0x374AA223, 0x374AA226,
    0x374AA229, 0x374AA234, 0x374AA237, 0x374AA251, 0x374AA263, 0x374AA268, 0x374AA311, 0x374AA314, 0x374AA319,
    0x374AA322, 0x374AA329, 0x374AA355, 0x379A1804, 0x379A1807, 0x38A90002, 0x38EE0000, 0x38EE0002, 0x38EE0039,
    0x391D1A04, 0x391D1A08, 0x391D1A0E, 0x391D1A13, 0x391D1A25, 0x391D1A2B, 0x391D1A34, 0x391D2017, 0x391D2A05,
};

bool IsReceiver(USHORT vid, USHORT pid) {
    UINT key = ((UINT)vid << 16) | pid;
    return std::find(std::begin(kReceivers), std::end(kReceivers), key) != std::end(kReceivers);
}

// Product string without link or device-type words: "AJAZZ 2.4G 8K" -> "AJAZZ", "2.4G Wireless Receiver" -> "",
// "Akko 2.4G Wireless Keyboard" -> "Akko" (the caller appends " wireless keyboard" / " wireless mouse").
std::wstring Brand(const std::wstring& product) {
    std::wistringstream in(product);
    std::wstring word, out;
    while (in >> word) {
        std::wstring u = word;
        for (auto& ch : u) ch = (wchar_t)std::towupper(ch);
        if (u == L"2.4G" || u == L"2.4GHZ" || u == L"8K" || u == L"4K" || u == L"2K" || u == L"1K" || u == L"RECEIVER" ||
            u == L"WIRELESS" || u == L"DONGLE" || u == L"USB" || u == L"KEYBOARD" || u == L"MOUSE")
            continue;
        out += (out.empty() ? L"" : L" ") + word;
    }
    return out;
}

bool QueryStatus(HidDev& d, std::vector<BYTE>& reply) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) Sleep(100);
        std::vector<BYTE> cmd(d.coll().featLen(), 0);
        cmd[1] = 0xF7;
        if (!d.SetFeature(cmd)) return false;
        Sleep(20);
        if (d.GetFeature(0, reply) && reply.size() >= 7 && (reply[2] | reply[3] | reply[4] | reply[5] | reply[6]) != 0) return true;
    }
    return false;
}

}  // namespace

void PollRongyuan(PollContext& ctx) {
    std::vector<std::pair<USHORT, USHORT>> products;
    for (const auto& c : ctx.colls)
        if (IsReceiver(c.vid, c.pid) && !ctx.IsClaimed(c.vid, c.pid) &&
            std::find(products.begin(), products.end(), std::make_pair(c.vid, c.pid)) == products.end())
            products.push_back({ c.vid, c.pid });

    for (auto [vid, pid] : products) {
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) { return c.iface == 2 && c.page() == 0xFFFF && c.usage() == 0x0002 && c.featLen() >= 7; })) {
            HidDev d;
            if (!d.OpenFeatureOnly(*c)) continue;
            std::vector<BYTE> r;
            if (!QueryStatus(d, r)) continue;
            std::wstring product = HidProduct(*c);
            bool generic = product.empty() || product.find(L"Receiver") != std::wstring::npos || product.find(L"2.4G") != std::wstring::npos;
            bool any = false;
            // Slot layout: [2] keyboard %, [3] mouse %, [4] keyboard offline, [5] mouse offline.
            for (int slot = 0; slot < 2; slot++) {
                bool keyboard = slot == 0;
                if (r[keyboard ? 4 : 5] != 0) continue;  // nothing connected in this slot
                BYTE level = r[keyboard ? 2 : 3];
                DeviceStatus s;
                s.key = L"rongyuan:" + Hex4(vid) + L":" + Hex4(pid) + (keyboard ? L":kb" : L":ms");
                std::wstring brand = Brand(product), upper = brand;
                for (auto& ch : upper) ch = (wchar_t)std::towupper(ch);
                bool otherBrand = !brand.empty() && upper.find(L"ATTACK SHARK") == std::wstring::npos;
                if (vid == 0x3151 && pid == 0x402D && !keyboard && !otherBrand) s.name = L"Attack Shark R3";
                else if (vid == 0x3151 && pid == 0x5038 && keyboard) s.name = L"MonsGeek keyboard";  // receiver says "MonsGeek 2.4G Wireless Keyboard"
                else if (generic && !brand.empty()) s.name = brand + (keyboard ? L" wireless keyboard" : L" wireless mouse");
                else if (vid == 0x3151 && pid == 0x5006 && keyboard && product.find(L"Akko") != std::wstring::npos)
                    s.name = L"Akko Air 01";
                else if (generic) s.name = keyboard ? L"Wireless keyboard" : L"Wireless mouse";
                else s.name = product;
                s.kind = keyboard ? DeviceKind::Keyboard : DeviceKind::Mouse;
                s.via = L"2.4 GHz receiver";

                // Retain the last battery reading while this keyboard is sleeping: the receiver then sets the
                // keyboard offline flag ([4]), so without a grace period the keyboard vanished a few minutes
                // after the last key press. Hardware-tested: MonsGeek 3151:5038, Akko 5075B Plus V2 3151:4011.
                if (vid == 0x3151 && (pid == 0x5038 || pid == 0x4011) && keyboard) {
                    s.graceSec = 1800;
                }

                if (level >= 1 && level <= 100) s.percent = level;
                ctx.out.push_back(s);
                any = true;
            }
            ctx.Claim(vid, pid);
            (void)any;
            break;
        }
    }
}
