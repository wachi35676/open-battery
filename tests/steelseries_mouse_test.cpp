// Exercise the real Aerox/Prime driver with a simulated HID transport; no hardware writes.
#include "../src/drivers/drivers.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>

namespace {
void Check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "Check failed: %s (line %d)\n", expression, line);
        std::exit(EXIT_FAILURE);
    }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// Simulated interface 3: every write queues `noise` (traffic from another app such as GG, when set),
// and the write numbered `replyOnWrite` also queues `reply`. Reads return the queued reports in order.
BYTE expectedCommand = 0xD2;
bool writeOk = true;
int writes = 0, replyOnWrite = 1;
std::vector<BYTE> reply, noise;
std::deque<std::vector<BYTE>> queued;
std::map<USHORT, std::vector<BYTE>> repliesByPid;

HidColl Collection(USHORT pid) {
    HidColl c;
    c.vid = 0x1038;
    c.pid = pid;
    c.iface = 3;
    c.caps.UsagePage = 0xFFC0;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = 65;
    return c;
}

PollContext Poll(USHORT pid, BYTE command, std::vector<BYTE> payload) {
    expectedCommand = command;
    writes = 0;
    reply = payload;
    if (!reply.empty()) reply.insert(reply.begin(), command);
    PollContext ctx;
    ctx.colls = { Collection(pid) };
    PollSteelSeries(ctx);
    return ctx;
}

struct Model { USHORT pid; bool wired; const wchar_t* name; const wchar_t* key; };
struct Sample { std::vector<BYTE> payload; int percent; bool charging, connected; };

void Expect(const Model& model, const Sample& sample) {
    BYTE command = model.wired ? 0x92 : 0xD2;
    auto ctx = Poll(model.pid, command, sample.payload);
    CHECK(ctx.out.size() == 1 && ctx.IsClaimed(0x1038, model.pid));
    const auto& s = ctx.out.front();
    if (s.percent != sample.percent || s.charging != sample.charging || s.connected != sample.connected) {
        std::fprintf(stderr, "PID %04X reply %02X %02X: got percent=%d charging=%d connected=%d; expected %d %d %d\n",
                     model.pid, sample.payload[0], sample.payload.size() > 1 ? sample.payload[1] : 0, s.percent,
                     s.charging, s.connected, sample.percent, sample.charging, sample.connected);
        std::exit(EXIT_FAILURE);
    }
    CHECK(s.name == model.name);
    CHECK(s.graceSec == 1800);  // the last level stays, dimmed, while the mouse is off or asleep
    CHECK(s.key == model.key);
    CHECK(s.kind == DeviceKind::Mouse);
    CHECK(s.via == (model.wired ? L"USB cable" : L"2.4 GHz receiver"));
}

void ExpectNothing(USHORT pid, BYTE command, std::vector<BYTE> payload) {
    auto ctx = Poll(pid, command, payload);
    CHECK(ctx.out.empty() && ctx.claimed.empty());
}

void ExpectOneMouse(const Model& receiver, const Model& cable,
                    std::vector<BYTE> wireless = { 0xD2, 0x0A }, std::vector<BYTE> wired = { 0x92, 0x94 }) {
    repliesByPid = { { receiver.pid, wireless }, { cable.pid, wired } };
    PollContext ctx;
    ctx.colls = { Collection(cable.pid), Collection(receiver.pid) };
    PollSteelSeries(ctx);
    CHECK(ctx.out.size() == 1);
    CHECK(ctx.IsClaimed(0x1038, receiver.pid) && ctx.IsClaimed(0x1038, cable.pid));
    const auto& s = ctx.out.front();
    CHECK(s.key == receiver.key && s.name == receiver.name);
    CHECK(s.via == L"USB cable" && s.percent == 95 && s.charging);
    CHECK(s.vid == 0x1038);
    repliesByPid.clear();
    // Unplugging the cable must keep the same key and return to the wireless level.
    Expect(receiver, { std::vector<BYTE>(wireless.begin() + 1, wireless.end()), 45, false, true });
}
}  // namespace

bool HidDev::Open(const HidColl& c) { c_ = c; h_ = reinterpret_cast<HANDLE>(1); return true; }
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
void HidDev::Flush() { queued.clear(); }
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    if (!repliesByPid.empty()) {
        auto it = repliesByPid.find(c_.pid);
        CHECK(it != repliesByPid.end());
        CHECK(data == std::vector<BYTE>({ 0x00, it->second[0] }));
        queued.push_back(it->second);
        return true;
    }
    CHECK(data == std::vector<BYTE>({ 0x00, expectedCommand }));
    if (!writeOk) return false;
    ++writes;
    if (!noise.empty()) queued.push_back(noise);
    if (writes == replyOnWrite && !reply.empty()) queued.push_back(reply);
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    CHECK(timeoutMs > 0 && timeoutMs <= 100);
    if (queued.empty()) {  // like a real read, nothing arrives before the timeout
        Sleep(timeoutMs);
        return -1;
    }
    out = queued.front();
    queued.pop_front();
    return (int)out.size();
}
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
int MapTo100(int v, int lo, int hi) { return Clamp100((v - lo) * 100 / (hi - lo)); }

int main() {
    // Step layout: [1] = (level/5 + 1) | charging<<7.
    const Model stepModels[] = {
        { 0x1838, false, L"SteelSeries Aerox 3 Wireless", L"steelseries:1838" },
        { 0x183A, true, L"SteelSeries Aerox 3 Wireless", L"steelseries:1838" },
        { 0x1878, false, L"SteelSeries Aerox 3 Wireless CS2 Dragon Lore Edition", L"steelseries:1878" },
        { 0x187A, true, L"SteelSeries Aerox 3 Wireless CS2 Dragon Lore Edition", L"steelseries:1878" },
        { 0x1852, false, L"SteelSeries Aerox 5 Wireless", L"steelseries:1852" },
        { 0x1854, true, L"SteelSeries Aerox 5 Wireless", L"steelseries:1852" },
        { 0x185C, false, L"SteelSeries Aerox 5 Wireless Destiny 2 Lightfall Edition", L"steelseries:185C" },
        { 0x185E, true, L"SteelSeries Aerox 5 Wireless Destiny 2 Lightfall Edition", L"steelseries:185C" },
        { 0x1858, false, L"SteelSeries Aerox 9 Wireless", L"steelseries:1858" },
        { 0x185A, true, L"SteelSeries Aerox 9 Wireless", L"steelseries:1858" },
        { 0x1840, false, L"SteelSeries Prime Wireless", L"steelseries:1840" },
        { 0x1842, true, L"SteelSeries Prime Wireless", L"steelseries:1840" },
        { 0x1860, false, L"SteelSeries Aerox 5 Wireless Diablo IV Edition", L"steelseries:1860" },
    };
    const Sample stepSamples[] = {
        { { 0x01 }, 0, false, true }, { { 0x13 }, 90, false, true }, { { 0x15 }, 100, false, true },
        { { 0x81 }, 0, true, true }, { { 0x93 }, 90, true, true }, { { 0x95 }, 100, true, true },
        // Captured on a Dragon Lore Edition: receiver on battery, then cable while charging.
        { { 0x0A, 0x00, 0x34, 0x2E, 0x31 }, 45, false, true },
        { { 0x8B, 0x00, 0x34, 0x2E, 0x31 }, 50, true, true }, { { 0x8C, 0x00 }, 55, true, true },
        // Out-of-range levels must stay unknown and never invent a charging state.
        { { 0xFF }, -1, false, true }, { { 0x7F }, -1, false, true },
        { { 0x16 }, -1, false, true }, { { 0x96 }, -1, false, true },
    };
    for (const auto& model : stepModels) {
        for (const auto& sample : stepSamples) Expect(model, sample);
        // Zero means off or asleep, even with the charging bit set: left out so the tray keeps the last level.
        ExpectNothing(model.pid, model.wired ? 0x92 : 0xD2, { 0x00 });
        ExpectNothing(model.pid, model.wired ? 0x92 : 0xD2, { 0x80 });
    }

    for (size_t i = 0; i + 1 < std::size(stepModels); i += 2) ExpectOneMouse(stepModels[i], stepModels[i + 1]);

    // Percent layout (Aerox 3 Wireless Gen 2): [1] 00 battery / 01 charging / 02 charged, [2] level.
    const Model gen2[] = { { 0x1890, false, L"SteelSeries Aerox 3 Wireless Gen 2", L"steelseries:1890" },
                           { 0x1892, true, L"SteelSeries Aerox 3 Wireless Gen 2", L"steelseries:1890" } };
    for (const auto& model : gen2) {
        // Captured on hardware in rivalcfg issue #276.
        Expect(model, { { 0x00, 0x61, 0x00 }, 97, false, true });
        Expect(model, { { 0x01, 0x56, 0x00 }, 86, true, true });
        Expect(model, { { 0x02, 0x64, 0x00 }, 100, true, true });
        Expect(model, { { 0x00, 0xFF, 0x00 }, -1, false, true });  // not measured yet
        Expect(model, { { 0x01, 0xFF, 0x00 }, -1, true, true });
        Expect(model, { { 0x01, 0x00, 0x00 }, 0, true, true });
        Expect(model, { { 0x00, 0x65, 0x00 }, -1, false, true });  // above 100: unknown
        // 00 00 through the receiver means the mouse is off (left out); on the cable it is a real 0%.
        BYTE command = model.wired ? 0x92 : 0xD2;
        if (model.wired) Expect(model, { { 0x00, 0x00, 0x00 }, 0, false, true });
        else ExpectNothing(model.pid, command, { 0x00, 0x00, 0x00 });
        ExpectNothing(model.pid, command, { 0x12, 0x00, 0x00 });  // a step-layout reply is not a Gen 2 status
        ExpectNothing(model.pid, command, { 0x01 });              // too short for the level byte
    }

    ExpectOneMouse(gen2[0], gen2[1], { 0xD2, 0x00, 0x2D }, { 0x92, 0x01, 0x5F });

    // Reports that do not echo the command are skipped (an acknowledgement, then the reply).
    noise = { 0x11, 0x00 };
    Expect(stepModels[2], { { 0x0A }, 45, false, true });
    // GG talking over the query: its reports keep coming and the receiver answers the third write.
    // Captured on an original Aerox 3: the installed one-read driver misread this as "off".
    noise = { 0x61, 0x00, 0x31, 0x31, 0x2E, 0x34, 0x00, 0x00 };
    Expect(stepModels[0], { { 0x0A, 0x31, 0x31, 0x2E, 0x34, 0x00, 0x00 }, 45, false, true });
    replyOnWrite = 3;
    Expect(stepModels[2], { { 0x0A }, 45, false, true });
    CHECK(writes == 3);
    Expect(gen2[0], { { 0x00, 0x61, 0x00 }, 97, false, true });
    // No echo before the deadline: nothing is reported or claimed, and the query was resent.
    ExpectNothing(0x1878, 0xD2, {});
    CHECK(writes >= 5 && writes <= 11);
    replyOnWrite = 1;
    // Mouse switched off: the Dragon Lore receiver answers 40 FF, which ends the wait at once.
    noise = { 0x40, 0xFF, 0x31, 0x34, 0x2E, 0x31 };
    ULONGLONG started = GetTickCount64();
    ExpectNothing(0x1878, 0xD2, {});
    CHECK(writes == 1 && GetTickCount64() - started < 100);
    noise.clear();

    // I/O failures.
    writeOk = false;
    ExpectNothing(0x1838, 0xD2, { 0x13 });
    CHECK(writes == 0);
    writeOk = true;
    ExpectNothing(0x1838, 0xD2, {});  // receiver never answers
    std::puts("SteelSeries mouse tests passed (step and Gen 2 layouts, 15 models, USB/wireless, battery bounds, "
              "charging, shared cable/receiver identity, off/asleep left out, echo filtering, resend, receiver NACK, I/O failures).");
}
