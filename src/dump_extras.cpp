// Extra sections for `--dump`: HID collections that could not be opened, every USB device (including
// ones without a HID interface) and the Bluetooth devices Windows knows about. Read-only, no network.
#include "dump_extras.h"

#include <windows.h>
#include <devpkey.h>
#include <setupapi.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <vector>

#include "devices.h"
#include "hid.h"

namespace {

const DEVPROPKEY kBusReportedDesc = { { 0x540B947E, 0x8B40, 0x45BC, { 0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2 } }, 4 };
const DEVPROPKEY kBtBattery = { { 0x104EA319, 0x6EE2, 0x4701, { 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5 } }, 2 };
const GUID kBluetoothClass = { 0xE0CBF06C, 0xCD8B, 0x4647, { 0xBB, 0x8A, 0x26, 0x3B, 0x43, 0xF0, 0xF9, 0x74 } };

// ---- device scan --------------------------------------------------------------------------------

std::wstring Upper(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towupper(c);
    return s;
}

std::wstring RegProp(HDEVINFO di, SP_DEVINFO_DATA& dd, DWORD prop) {
    wchar_t buf[512] = {};
    DWORD type = 0;
    if (SetupDiGetDeviceRegistryPropertyW(di, &dd, prop, &type, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr) &&
        (type == REG_SZ || type == REG_MULTI_SZ))
        return buf;
    return L"";
}

std::wstring StrProp(HDEVINFO di, SP_DEVINFO_DATA& dd, const DEVPROPKEY& key) {
    wchar_t buf[512] = {};
    DEVPROPTYPE type = 0;
    if (SetupDiGetDevicePropertyW(di, &dd, &key, &type, (PBYTE)buf, sizeof(buf) - sizeof(wchar_t), nullptr, 0) &&
        type == DEVPROP_TYPE_STRING)
        return buf;
    return L"";
}

std::wstring DeviceName(HDEVINFO di, SP_DEVINFO_DATA& dd) {
    std::wstring n = StrProp(di, dd, kBusReportedDesc);  // what the device itself reports, e.g. "Pulsar 8K Dongle"
    if (n.empty()) n = RegProp(di, dd, SPDRP_FRIENDLYNAME);
    if (n.empty()) n = RegProp(di, dd, SPDRP_DEVICEDESC);
    return n;
}

// Reads the vendor/product id from a USB instance id or HID path ("VID_046D&PID_C547") or a Bluetooth
// HID path ("_VID&0002046D_PID&B023", where the first four digits name the id's source).
bool ParseIds(const std::wstring& id, USHORT& vid, USHORT& pid) {
    std::wstring u = Upper(id);
    size_t v = u.find(L"VID_"), p = u.find(L"PID_");
    if (v != std::wstring::npos && p != std::wstring::npos) {
        vid = (USHORT)wcstoul(u.substr(v + 4, 4).c_str(), nullptr, 16);
        pid = (USHORT)wcstoul(u.substr(p + 4, 4).c_str(), nullptr, 16);
        return vid != 0;
    }
    v = u.find(L"VID&");
    p = u.find(L"PID&");
    if (v != std::wstring::npos && p != std::wstring::npos && u.size() >= v + 12) {
        vid = (USHORT)wcstoul(u.substr(v + 8, 4).c_str(), nullptr, 16);
        pid = (USHORT)wcstoul(u.substr(p + 4, 4).c_str(), nullptr, 16);
        return vid != 0;
    }
    return false;
}

int InterfaceOf(const std::wstring& path) {
    std::wstring u = Upper(path);
    size_t mi = u.find(L"&MI_");
    return mi == std::wstring::npos ? -1 : (int)wcstol(u.c_str() + mi + 4, nullptr, 16);
}

struct UsbDevice {
    USHORT vid = 0, pid = 0;
    std::wstring name;
    std::map<std::wstring, int> interfaces;  // "HIDClass", "MEDIA (usbaudio)", "Ports (usbser)" ... -> count
    bool hub = false;
};

// Every present USB device, including the ones that have no HID interface (audio-only or WinUSB dongles,
// serial adapters): those never show up in the HID list, which is why some devices were missing from --dump.
std::vector<UsbDevice> EnumUsb() {
    std::map<DWORD, UsbDevice> byId;
    HDEVINFO di = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (di == INVALID_HANDLE_VALUE) return {};
    SP_DEVINFO_DATA dd{ sizeof(dd) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
        wchar_t idBuf[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(di, &dd, idBuf, 511, nullptr)) continue;
        USHORT vid = 0, pid = 0;
        if (!ParseIds(idBuf, vid, pid)) continue;  // root hubs and host controllers have no ids
        UsbDevice& d = byId[((DWORD)vid << 16) | pid];
        d.vid = vid;
        d.pid = pid;
        std::wstring cls = RegProp(di, dd, SPDRP_CLASS), svc = RegProp(di, dd, SPDRP_SERVICE);
        std::wstring label = (cls.empty() ? L"unknown" : cls) + (svc.empty() ? L"" : L" (" + svc + L")");
        bool isInterface = Upper(idBuf).find(L"&MI_") != std::wstring::npos;
        if (!isInterface) {
            if (d.name.empty()) d.name = DeviceName(di, dd);
            if (Upper(svc).find(L"HUB") != std::wstring::npos) d.hub = true;
            if (Upper(cls) != L"USB") d.interfaces[label]++;  // single-function device: the device is its own interface
        } else {
            d.interfaces[label]++;
        }
    }
    SetupDiDestroyDeviceInfoList(di);
    std::vector<UsbDevice> out;
    for (auto& [k, d] : byId)
        if (!d.hub) out.push_back(d);
    return out;
}

struct Unopened {
    USHORT vid = 0, pid = 0;
    int iface = -1;
    std::wstring name;
};

// HID collections EnumHid() could not open (held exclusively by another program, or access denied).
// EnumHid() skips them, so before this they were simply missing from --dump.
std::vector<Unopened> UnopenedHid(const std::vector<HidColl>& opened) {
    std::set<std::wstring> seen;
    for (const auto& c : opened) seen.insert(Upper(c.path));
    std::vector<Unopened> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO di = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &hidGuid, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dd)) continue;
        if (seen.count(Upper(det->DevicePath))) continue;
        Unopened u;
        if (!ParseIds(det->DevicePath, u.vid, u.pid)) continue;
        u.iface = InterfaceOf(det->DevicePath);
        u.name = DeviceName(di, dd);
        out.push_back(u);
    }
    SetupDiDestroyDeviceInfoList(di);
    return out;
}

}  // namespace

namespace diagnostics {

std::wstring DumpExtras() {
    std::wstring out;
    auto unopened = UnopenedHid(EnumHid());
    if (!unopened.empty()) {
        out += L"\nHID collections that could not be opened (in use by another program):\n";
        for (const auto& u : unopened) {
            wchar_t line[64];
            swprintf(line, 64, L"%04X:%04X  %2d  ", u.vid, u.pid, u.iface);
            out += line + u.name + L"\n";
        }
    }
    out += L"\nUSB devices, including ones without a HID interface (VID:PID  name  [interfaces]):\n";
    for (const auto& u : EnumUsb()) {
        wchar_t ids[16];
        swprintf(ids, 16, L"%04X:%04X", u.vid, u.pid);
        std::wstring line = std::wstring(ids) + L"  " + (u.name.empty() ? L"(no name)" : u.name) + L"  [";
        bool first = true;
        for (const auto& [label, n] : u.interfaces) {
            line += (first ? L"" : L", ") + label + (n > 1 ? L" x" + std::to_wstring(n) : L"");
            first = false;
        }
        out += line + L"]\n";
    }
    out += L"\nBluetooth devices Windows knows about:\n";
    HDEVINFO di = SetupDiGetClassDevsW(&kBluetoothClass, nullptr, nullptr, DIGCF_PRESENT);
    if (di != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd{ sizeof(dd) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(di, i, &dd); i++) {
            wchar_t idBuf[512] = {};
            SetupDiGetDeviceInstanceIdW(di, &dd, idBuf, 511, nullptr);
            if (Upper(idBuf).rfind(L"BTH", 0) != 0) continue;  // the radio itself and its services
            std::wstring name = RegProp(di, dd, SPDRP_FRIENDLYNAME);
            if (name.empty()) continue;
            BYTE level = 0;
            DEVPROPTYPE type = 0;
            bool battery = SetupDiGetDevicePropertyW(di, &dd, &kBtBattery, &type, &level, sizeof(level), nullptr, 0) && type == DEVPROP_TYPE_BYTE;
            out += L"  " + name + (battery ? L"  (battery reported by Windows: " + std::to_wstring(level) + L"%)" : L"") + L"\n";
        }
        SetupDiDestroyDeviceInfoList(di);
    }
    return out;
}

}  // namespace diagnostics
