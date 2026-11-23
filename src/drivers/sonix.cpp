// SONiX keyboards on their 2.4 GHz dongle: AJAZZ AK820 MAX ("AJAZZ AK820 MAX 2.4G", 05AC:024F, tested),
// AULA F108 Pro (same 05AC:024F dongle), AJAZZ AK980 V2 (0C45:FEFE) and AJAZZ AK820 Pro (0C45:FDFD,
// some units ship a 0C45:FEFE "2.4G Dongle"). The 05AC dongle reuses Apple's vendor id, so the dongles
// are also matched by their vendor collection: usage page FF60 / usage 61, 33-byte input
// and output reports (report id 0 + 32 bytes). These dongles require interface 3;
// EPOMAKER P75 (342D:E479, request #2525, hardware untested) uses interface 1;
// RSQ_20055_2.4G (342D:E411, issue #3808, untested) reports interface 1, FF60:0061, 33/33;
// Galaxy100 (342D:E489, request #3802, hardware untested) reports interface 2.
//   request [20 01 00 ... sum]          sum = byte sum of the 31 bytes before it
//   reply   [20 01 00 level ...]        level in percent
// A reply is recognised by its first three bytes (the echoed command), as the AK820 Pro tools do; its
// checksum is not checked. The keyboard only reports its level while it is switched to 2.4G mode.
// The protocol is read-only.
// MCHOSE K87 receivers on the same 05AC:024F id expose a different layout (the BY collection on
// interface 1, no FF60 on interface 3); aula.cpp runs first and reads those.
// From github.com/donglover69/sonix-keyboard-studio (README, "2.4G dongles"); the AK820 Pro receiver
// 0C45:FDFD and the echo match from github.com/deddroid/ak820-rs (src/device.rs, src/protocol.rs) and
// github.com/usharik/ak820-studio (Sources/AK820Kit/Keyboard.swift).
#include "drivers.h"

namespace {

// iface < 0: match the exact vendor collection on any interface.
struct Dongle { USHORT vid, pid; int iface; };
const Dongle kDongles[] = {
    { 0x05AC, 0x024F, 3 },
    { 0x0C45, 0xFEFE, 3 },
    { 0x0C45, 0xFDFD, 3 },
    { 0x342D, 0xE479, -1 },  // EPOMAKER P75 (hardware untested)
    { 0x342D, 0xE411, 1 },   // RSQ_20055_2.4G (issue #3808, hardware untested)
    { 0x342D, 0xE489, 2 },   // EPOMAKER Galaxy100 (issue #3802, hardware untested)
    { 0x342D, 0xE485, -1 },  // RK Royal Kludge R75 (PR #2593, hardware untested)
    { 0x342D, 0xE4BC, -1 },  // Redragon Arlokks K742B (PR #2594, hardware untested)
    { 0x342D, 0xE4C2, -1 },  // M1 V5 VIA (PR #2595, hardware untested)
    { 0x342D, 0xE407, -1 },  // Lucky65V2 (PR #2597, hardware untested)
    { 0x388D, 0x0003, -1 },  // Lofree Flow2 (PR #2598, hardware untested)
    { 0x0DB0, 0x0CA0, -1 },  // MSI FORGE TKL W FRIEREN EDITION (PR #2599, hardware untested)
    { 0x36B0, 0x30BD, -1 },  // Essential75 (PR #2607, hardware untested)
    { 0x342D, 0xE51A, -1 },  // NUT65 (PR #2608, hardware untested)
    { 0x342D, 0xE487, -1 },  // RK 2.4G Dongle (PR #2609, hardware untested)
    { 0x342D, 0xE537, -1 },  // WEIKAV MAX102 (PR #2610, hardware untested)
    { 0x388D, 0x0002, -1 },  // Lofree Flow2 (PR #2612, hardware untested)
    { 0x19F5, 0x3245, -1 },  // NuPhy Air75V2 (PR #2613, hardware untested)
    { 0x362D, 0xD028, -1 },  // SKN Qinglong (PR #2614, hardware untested)
    { 0x362D, 0xD030, -1 },  // Lemokey Link (PR #2616, hardware untested)
    { 0x362D, 0xD101, -1 },  // Wireless Link (PR #2617, hardware untested)
    { 0x342D, 0xE49F, -1 },  // Spade65-QMK-2.4GHz (PR #2619, hardware untested)
};

const size_t kLen = 32;  // payload bytes after the report id; the last one is the checksum

BYTE Sum(const std::vector<BYTE>& b, size_t from, size_t n) {
    unsigned s = 0;
    for (size_t i = from; i < from + n; i++) s += b[i];
    return (BYTE)(s & 0xFF);
}

// Returns the level byte, or -1 when the dongle did not answer.
int QueryLevel(HidDev& d) {
    std::vector<BYTE> req(1 + kLen, 0);  // [0] = report id 0
    req[1] = 0x20;
    req[2] = 0x01;
    req[kLen] = Sum(req, 1, kLen - 1);
    d.Flush();
    if (!d.Write(req)) return -1;
    ULONGLONG end = GetTickCount64() + 500;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) return -1;
        std::vector<BYTE> r;
        int n = d.Read(r, left);  // report id 0 is stripped: r[0] is the first payload byte
        if (n < 0) return -1;
        if (n < 4 || r[0] != 0x20 || r[1] != 0x01 || r[2] != 0x00) continue;
        return r[3];
    }
}

std::wstring KeyboardName(const HidColl& c) {
    std::wstring name = HidProduct(c);
    const std::wstring suffix = L" 2.4G";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    return name.empty() ? L"Wireless keyboard" : name;
}

}  // namespace

void PollSonix(PollContext& ctx) {
    for (const Dongle& m : kDongles) {
        if (ctx.IsClaimed(m.vid, m.pid)) continue;
        for (const HidColl* c : ctx.Find(m.vid, m.pid, [&m](const HidColl& c) {
                 return (m.iface < 0 || c.iface == m.iface) && c.page() == 0xFF60 && c.usage() == 0x0061 &&
                        c.outLen() == 1 + kLen && c.inLen() == 1 + kLen;
             })) {
            HidDev d;
            if (!d.Open(*c)) continue;
            // The first request can go unanswered while the 2.4 GHz link wakes up; retry once.
            int level = QueryLevel(d);
            if (level < 0) level = QueryLevel(d);
            if (level < 0) continue;
            DeviceStatus s;
            s.key = L"sonix:" + Hex4(m.vid) + L":" + Hex4(m.pid);
            s.name = KeyboardName(*c);
            s.kind = DeviceKind::Keyboard;
            s.via = L"2.4 GHz receiver";
            s.connected = level >= 1 && level <= 100;  // 0: keyboard off, out of range or in another mode
            if (s.connected) s.percent = level;
            s.graceSec = 1800;  // keep the last value if a sleeping keyboard stops answering
            ctx.Add(s, m.vid, m.pid);
            break;
        }
    }
}
