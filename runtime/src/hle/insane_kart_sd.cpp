// mkw100: Insane Kart Wii's SD file API (docs/mkw100/insane-kart.md).
//
// IKW 3.x does all of its own file I/O (settings save, ghosts, crash reports, Config.pul's
// leaderboards) through a function table its launcher provides: the launcher loads
// runtime-ext-P.dol to 0x81744260, and IKW's module holds a pointer to that DOL's table
// (0x81782E00) in its data. MKW100 has no launcher, so this file gives IKW the same table: twelve
// native functions over the host folder that holds IKWv3\ (the virtual SD card the runtime already
// uses for the pack), and IKW's pointer is set to it after the module is loaded.
//
// The table, as IKW's I/O class uses it (newlib-style calls, -1 or nonzero on failure):
//   +00 open(FILE*, path, flags) -> 0 / -1   FILE word 0 = the file size (IKW's GetFileSize)
//   +04 close(FILE*)               +08 read(FILE*, buf, len)    +0C write(FILE*, buf, len)
//   +10 rename(old, new) -> 0      +14 stat(path, st) -> 0, st+8 = mode (0x4000 dir, 0x8000 file)
//   +18 mkdir(path) -> 0           +1C opendir(DIR*, path) -> DIR* / 0
//   +20 readdir(DIR*, name, st) -> 0 while there are entries        +24 closedir(DIR*)
//   +28 lseek(FILE*, offset, whence) -> position                    +2C errno() (0x11 = EEXIST)
// flags: O_RDONLY 0, O_WRONLY 1, O_RDWR 2, O_APPEND 0x8, O_CREAT 0x200, O_TRUNC 0x400.
//
// Writes never go through a link: the installer mirrors IKWv3\ as junctions to the user's download,
// and only folders created in the stage (save\, Ghosts\, ...) may be written.

#include "hle/mod_build.h"
#include "hle/storage/riivolution.h"
#include "abi_bridge.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetFileAttributesW(const wchar_t* path);
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kIkwApiPointer = 0x81847550;    // IKW 3.4.1 module data: the table pointer
constexpr uint32_t kLauncherTable = 0x81782E00;    // its value: runtime-ext-P.dol's table
constexpr uint32_t kTable = 0x82DF0000;            // MKW100's table (array shadow span 13, unused)
// Native i is registered at kFunctions + 4 * i: the unused top of the Kamek module window, which
// guest_versions.inc maps to itself in every region (registrations must have a counterpart there).
constexpr uint32_t kFunctions = 0x819FF100;
constexpr uint32_t kEntries = 12;

constexpr int kEEXIST = 0x11;
constexpr int kENOENT = 2;
constexpr int kEACCES = 13;
constexpr int kEBADF = 9;

std::mutex g_lock;
int g_errno = 0;
uint32_t g_nextHandle = 1;
std::map<uint32_t, std::FILE*> g_files;                      // handle (FILE word 1) -> file
struct Dir { std::vector<std::pair<std::string, bool>> entries; size_t next = 0; };
std::map<uint32_t, Dir> g_dirs;                              // handle (DIR word 0) -> listing

// The virtual SD card: the folder above the overlay root that carries IKW's XML.
const fs::path& SdRoot() {
    static const fs::path root = [] {
        for (const auto& overlay : RuntimeRiivolution::Overlays())
            if (overlay.patches) return overlay.root.parent_path();
        return fs::path{};
    }();
    return root;
}

std::string Utf8(const fs::path& p) {
    const auto text = p.u8string();
    return std::string(text.begin(), text.end());
}

std::string GuestString(uint32_t address, size_t limit = 512) {
    std::string text;
    if (!address) return text;
    for (size_t i = 0; i < limit; ++i) {
        const char c = static_cast<char>(Memory::Read8(address + static_cast<uint32_t>(i)));
        if (!c) break;
        text.push_back(c);
    }
    return text;
}

// A guest SD path ("/IKWv3/save/...") on the host; empty for paths that leave the SD card.
fs::path HostPath(const std::string& guest) {
    if (SdRoot().empty()) return {};
    fs::path rel = fs::u8path(guest).relative_path();
    for (const auto& part : rel)
        if (part == "..") return {};
    return SdRoot() / rel;
}

bool IsLink(const fs::path& p) {
#ifdef _WIN32
    // Junctions are reparse points, which libc++ need not report as symlinks.
    const unsigned long attributes = GetFileAttributesW(p.wstring().c_str());
    return attributes != 0xFFFFFFFFul && (attributes & 0x400ul) != 0;  // FILE_ATTRIBUTE_REPARSE_POINT
#else
    std::error_code ec;
    return fs::is_symlink(fs::symlink_status(p, ec));
#endif
}

// True when no folder between the SD root and the path (nor the file itself) is a junction or a
// link, so writing there cannot reach the user's download.
bool Writable(const fs::path& guestRelative) {
    fs::path p = SdRoot();
    for (const auto& part : guestRelative) {
        p /= part;
        if (IsLink(p)) return false;
    }
    return true;
}
bool Writable(const std::string& guest) { return Writable(fs::u8path(guest).relative_path()); }

std::FILE* OpenHost(const fs::path& host, const char* mode) {
#ifdef _WIN32
    const std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(host.wstring().c_str(), wmode.c_str());
#else
    return std::fopen(host.string().c_str(), mode);
#endif
}

std::FILE* File(uint32_t fileStruct) {
    const auto it = g_files.find(Memory::Read32(fileStruct + 4));
    return it == g_files.end() ? nullptr : it->second;
}

void SetReturn(CpuContext* ctx, int32_t value) { ctx->gpr[3] = static_cast<uint32_t>(value); }

}  // namespace

extern "C" void InsaneKartSd_Open(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const uint32_t fileStruct = ctx->gpr[3];
    const std::string guest = GuestString(ctx->gpr[4]);
    const uint32_t flags = ctx->gpr[5];
    const fs::path host = HostPath(guest);
    const uint32_t access = flags & 3;
    std::error_code ec;
    const bool exists = !host.empty() && fs::is_regular_file(host, ec);
    std::FILE* f = nullptr;
    if (host.empty()) {
        g_errno = kENOENT;
    } else if (access == 0) {
        if (exists) f = OpenHost(host, "rb");
        if (!f) g_errno = kENOENT;
    } else if (!Writable(guest)) {
        g_errno = kEACCES;
        RT_LOGF("insane-kart", "refused to write %s (a link into the download)\n", guest.c_str());
    } else if (exists) {
        f = OpenHost(host, (flags & 0x400) ? "w+b" : "r+b");
    } else if (flags & 0x200) {
        f = OpenHost(host, "w+b");
    } else {
        g_errno = kENOENT;
    }
    static const bool trace = std::getenv("MKW100_TRACE_IKW_SD") != nullptr;
    if (trace) RT_LOGF("insane-kart", "sd open %s flags 0x%X -> %s\n", guest.c_str(), flags, f ? "ok" : "failed");
    if (!f) {
        SetReturn(ctx, -1);
        return;
    }
    if (flags & 0x8) std::fseek(f, 0, SEEK_END);
    const uint32_t handle = g_nextHandle++;
    g_files[handle] = f;
    std::error_code sizeEc;
    const auto size = fs::file_size(host, sizeEc);
    Memory::Write32(fileStruct, sizeEc ? 0u : static_cast<uint32_t>(size));
    Memory::Write32(fileStruct + 4, handle);
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Close(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const uint32_t handle = Memory::Read32(ctx->gpr[3] + 4);
    if (const auto it = g_files.find(handle); it != g_files.end()) {
        std::fclose(it->second);
        g_files.erase(it);
        Memory::Write32(ctx->gpr[3] + 4, 0);
        SetReturn(ctx, 0);
        return;
    }
    g_errno = kEBADF;
    SetReturn(ctx, -1);
}

extern "C" void InsaneKartSd_Read(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::FILE* f = File(ctx->gpr[3]);
    const uint32_t buffer = ctx->gpr[4], length = ctx->gpr[5];
    if (!f) { g_errno = kEBADF; SetReturn(ctx, -1); return; }
    std::vector<uint8_t> data(length);
    const size_t got = length ? std::fread(data.data(), 1, length, f) : 0;
    for (size_t i = 0; i < got; ++i) Memory::Write8(buffer + static_cast<uint32_t>(i), data[i]);
    SetReturn(ctx, static_cast<int32_t>(got));
}

extern "C" void InsaneKartSd_Write(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::FILE* f = File(ctx->gpr[3]);
    const uint32_t buffer = ctx->gpr[4], length = ctx->gpr[5];
    if (!f) { g_errno = kEBADF; SetReturn(ctx, -1); return; }
    std::vector<uint8_t> data(length);
    for (uint32_t i = 0; i < length; ++i) data[i] = Memory::Read8(buffer + i);
    const size_t put = length ? std::fwrite(data.data(), 1, length, f) : 0;
    std::fflush(f);
    SetReturn(ctx, static_cast<int32_t>(put));
}

extern "C" void InsaneKartSd_Rename(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string fromGuest = GuestString(ctx->gpr[3]), toGuest = GuestString(ctx->gpr[4]);
    const fs::path from = HostPath(fromGuest), to = HostPath(toGuest);
    std::error_code ec;
    if (from.empty() || to.empty() || !Writable(fromGuest) || !Writable(toGuest)) { g_errno = kEACCES; SetReturn(ctx, -1); return; }
    fs::rename(from, to, ec);
    if (ec) { g_errno = kENOENT; SetReturn(ctx, -1); return; }
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Stat(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const fs::path host = HostPath(GuestString(ctx->gpr[3]));
    const uint32_t st = ctx->gpr[4];
    std::error_code ec;
    const auto status = host.empty() ? fs::file_status{} : fs::status(host, ec);
    if (host.empty() || ec || !fs::exists(status)) { g_errno = kENOENT; SetReturn(ctx, -1); return; }
    for (uint32_t i = 0; i < 0x58; i += 4) Memory::Write32(st + i, 0);
    const bool dir = fs::is_directory(status);
    Memory::Write32(st + 8, dir ? 0x41FFu : 0x81B6u);
    if (!dir) {
        const auto size = fs::file_size(host, ec);
        Memory::Write32(st + 0x14, ec ? 0u : static_cast<uint32_t>(size));
    }
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Mkdir(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const std::string guest = GuestString(ctx->gpr[3]);
    const fs::path host = HostPath(guest);
    std::error_code ec;
    if (host.empty()) { g_errno = kENOENT; SetReturn(ctx, -1); return; }
    if (fs::exists(host, ec)) { g_errno = kEEXIST; SetReturn(ctx, -1); return; }
    if (!Writable(guest)) { g_errno = kEACCES; SetReturn(ctx, -1); return; }
    if (!fs::create_directory(host, ec) || ec) { g_errno = kENOENT; SetReturn(ctx, -1); return; }
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Opendir(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const uint32_t dirStruct = ctx->gpr[3];
    const fs::path host = HostPath(GuestString(ctx->gpr[4]));
    std::error_code ec;
    if (host.empty() || !fs::is_directory(host, ec)) { g_errno = kENOENT; SetReturn(ctx, 0); return; }
    Dir dir;
    for (const auto& entry : fs::directory_iterator(host, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code entryEc;
        dir.entries.emplace_back(Utf8(entry.path().filename()), entry.is_directory(entryEc));
    }
    std::sort(dir.entries.begin(), dir.entries.end());  // the same listing every run
    const uint32_t handle = g_nextHandle++;
    g_dirs[handle] = std::move(dir);
    Memory::Write32(dirStruct, handle);
    SetReturn(ctx, static_cast<int32_t>(dirStruct));
}

extern "C" void InsaneKartSd_Readdir(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    const auto it = g_dirs.find(Memory::Read32(ctx->gpr[3]));
    const uint32_t name = ctx->gpr[4], st = ctx->gpr[5];
    if (it == g_dirs.end() || it->second.next >= it->second.entries.size()) { SetReturn(ctx, -1); return; }
    const auto& [entryName, isDir] = it->second.entries[it->second.next++];
    const size_t n = std::min<size_t>(entryName.size(), 0x2FF);
    for (size_t i = 0; i < n; ++i) Memory::Write8(name + static_cast<uint32_t>(i), static_cast<uint8_t>(entryName[i]));
    Memory::Write8(name + static_cast<uint32_t>(n), 0);
    if (st) Memory::Write32(st + 8, isDir ? 0x41FFu : 0x81B6u);
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Closedir(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_dirs.erase(Memory::Read32(ctx->gpr[3]));
    SetReturn(ctx, 0);
}

extern "C" void InsaneKartSd_Lseek(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    std::FILE* f = File(ctx->gpr[3]);
    if (!f) { g_errno = kEBADF; SetReturn(ctx, -1); return; }
    const int whence = ctx->gpr[5] == 1 ? SEEK_CUR : ctx->gpr[5] == 2 ? SEEK_END : SEEK_SET;
    if (std::fseek(f, static_cast<long>(static_cast<int32_t>(ctx->gpr[4])), whence) != 0) { SetReturn(ctx, -1); return; }
    SetReturn(ctx, static_cast<int32_t>(std::ftell(f)));
}

extern "C" void InsaneKartSd_Errno(CpuContext* ctx) {
    std::lock_guard<std::mutex> lock(g_lock);
    SetReturn(ctx, g_errno);
}

REGISTER_NATIVE_FUNCTION_AS(0x819FF100, InsaneKartSd_Open, "InsaneKartSd_Open");
REGISTER_NATIVE_FUNCTION_AS(0x819FF104, InsaneKartSd_Close, "InsaneKartSd_Close");
REGISTER_NATIVE_FUNCTION_AS(0x819FF108, InsaneKartSd_Read, "InsaneKartSd_Read");
REGISTER_NATIVE_FUNCTION_AS(0x819FF10C, InsaneKartSd_Write, "InsaneKartSd_Write");
REGISTER_NATIVE_FUNCTION_AS(0x819FF110, InsaneKartSd_Rename, "InsaneKartSd_Rename");
REGISTER_NATIVE_FUNCTION_AS(0x819FF114, InsaneKartSd_Stat, "InsaneKartSd_Stat");
REGISTER_NATIVE_FUNCTION_AS(0x819FF118, InsaneKartSd_Mkdir, "InsaneKartSd_Mkdir");
REGISTER_NATIVE_FUNCTION_AS(0x819FF11C, InsaneKartSd_Opendir, "InsaneKartSd_Opendir");
REGISTER_NATIVE_FUNCTION_AS(0x819FF120, InsaneKartSd_Readdir, "InsaneKartSd_Readdir");
REGISTER_NATIVE_FUNCTION_AS(0x819FF124, InsaneKartSd_Closedir, "InsaneKartSd_Closedir");
REGISTER_NATIVE_FUNCTION_AS(0x819FF128, InsaneKartSd_Lseek, "InsaneKartSd_Lseek");
REGISTER_NATIVE_FUNCTION_AS(0x819FF12C, InsaneKartSd_Errno, "InsaneKartSd_Errno");

namespace {

void InsaneKartSd_Install() {
    if (!ModBuild::IsKnownInsaneKart()) return;
    for (uint32_t i = 0; i < kEntries; ++i) {
        Memory::Write32(kTable + 4 * i, kFunctions + 4 * i);
    }
    const uint32_t was = Memory::Read32(kIkwApiPointer);
    if (was != kLauncherTable) {
        RT_LOGF("insane-kart", "WARNING: 0x%08X holds 0x%08X, not the launcher table; SD API not installed\n",
                kIkwApiPointer, was);
        return;
    }
    Memory::Write32(kIkwApiPointer, kTable);
    RT_LOGF("insane-kart", "launcher SD API provided natively (SD card: %s)\n", Utf8(SdRoot()).c_str());
}

const bool kRegistered = [] {
    RecompMod::RegisterPostRelInitializer(&InsaneKartSd_Install);
    return true;
}();

}  // namespace
