// mkw100 `--race-auto`: a scripted human racer for headless tests.
//
// `--race-cpu` hands player 0 to the CPU driver (KartAIController::IsCPU and Kart::Link::IsCPU
// report a CPU), so the race has no real player at all and every code path that only runs for one
// is never tested. Two crashes came from such paths: AIRankManager walks over its real-player and
// group state (35b3b1c) only run with a human among the CPUs.
//
// With `--race-auto` player 0 stays a real local racer on GameCube port 1, and this fills that
// port's PADStatus each frame instead of a host controller: connected, accelerate held once the
// race has started, the stick steering at the midpoint of the checkpoint two ahead of the kart's
// current one (CKPT; the CPUs' ENPT route when that fails: following the route alone was 3x
// slower on Luigi Circuit), the item button pressed every few seconds, and a short reverse when the kart has not
// moved for a while. The race stays deterministic
// with the same seeds `--race-cpu` uses, so its result hash is a regression baseline.

#include "auto_driver.h"

#include "boot_race.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kRaceinfoGlobal = 0x809BD730;     // Raceinfo*: players** at +0xC, stage at +0x28
constexpr uint32_t kRaceinfoStage = 0x28;            // u32, 2 once the race is running
constexpr uint32_t kRaceinfoPlayerCheckpoint = 0xA;  // u16, the racer's current checkpoint
constexpr uint32_t kKartManager = 0x809C18F8;        // Kart::Manager*: KartPlayer** at +0x20
constexpr uint32_t kKmpManager = 0x809BD6E8;         // KMP::Manager*: Section<CKPT>* at +0x20
constexpr uint32_t kKmpCkptSection = 0x20;           // Holder** at +0, u16 count at +4
constexpr uint32_t kCkptRight = 0x8;                 // CKPT: Vec2 left (x, z) at +0, Vec2 right
constexpr uint32_t kCkptNext = 0x13;                 // u8, 0xFF on the last checkpoint
constexpr uint32_t kLookAhead = 2;                   // checkpoints ahead of the current one
constexpr uint32_t kKmpEnphSection = 0xC;            // PointGroup {u8 start, length, prev[6], next[6]}
constexpr uint32_t kKmpEnptSection = 0x10;           // ENPT: Vec3 position, float width (0x14)
constexpr uint32_t kRouteAhead = 3;                  // route points ahead of the nearest one

constexpr uint32_t kItemPeriod = 240;                // frames between item presses
constexpr uint32_t kStuckFrames = 90;                // moved less than kStuckDistance in this long: back up
constexpr float kStuckDistance = 200.0f;
constexpr uint32_t kReverseFrames = 60;

uint32_t g_frame = 0;
uint32_t g_lastProgressFrame = 0;
float g_stuckX = 0, g_stuckZ = 0;
uint32_t g_reverseUntil = 0;

bool InGuestRam(uint32_t address) {
    return (address >= 0x80000000u && address < 0x81800000u) || (address >= 0x90000000u && address < 0x98000000u);
}

// Kart::Link::GetKartPosition (0x8059020C) and GetMtx (0x80590264), read directly.
bool KartPose(uint32_t kart, float& x, float& z, float& forwardX, float& forwardZ) {
    const uint32_t pointers = Memory::Read32(kart);
    if (!InGuestRam(pointers)) return false;
    const uint32_t body = Memory::Read32(pointers + 8);
    if (!InGuestRam(body)) return false;
    const uint32_t holder = Memory::Read32(body + 0x90);
    if (!InGuestRam(holder)) return false;
    const uint32_t physics = Memory::Read32(holder + 4);
    if (!InGuestRam(physics)) return false;
    x = Memory::ReadFloat32(physics + 0x68);
    z = Memory::ReadFloat32(physics + 0x70);
    const uint32_t mtx = holder + 0x9C;  // Mtx34, the kart's +Z axis is its forward
    forwardX = Memory::ReadFloat32(mtx + 0x08);
    forwardZ = Memory::ReadFloat32(mtx + 0x28);
    return std::isfinite(x) && std::isfinite(z) && std::isfinite(forwardX) && std::isfinite(forwardZ);
}

// Midpoint of checkpoint `index`'s left and right points, `ahead` links further along.
bool CheckpointTarget(uint32_t index, uint32_t ahead, float& x, float& z) {
    const uint32_t manager = Memory::Read32(kKmpManager);
    if (!InGuestRam(manager)) return false;
    const uint32_t section = Memory::Read32(manager + kKmpCkptSection);
    if (!InGuestRam(section)) return false;
    const uint32_t count = Memory::Read16(section + 4);
    const uint32_t holders = Memory::Read32(section);
    if (count == 0 || index >= count || !InGuestRam(holders)) return false;
    for (uint32_t i = 0; i < ahead; ++i) {
        const uint32_t raw = Memory::Read32(Memory::Read32(holders + index * 4));
        const uint32_t next = Memory::Read8(raw + kCkptNext);
        index = next >= count ? 0 : next;
    }
    const uint32_t raw = Memory::Read32(Memory::Read32(holders + index * 4));
    x = 0.5f * (Memory::ReadFloat32(raw) + Memory::ReadFloat32(raw + kCkptRight));
    z = 0.5f * (Memory::ReadFloat32(raw + 4) + Memory::ReadFloat32(raw + kCkptRight + 4));
    return true;
}

uint32_t Raw(uint32_t section, uint32_t index) {
    return Memory::Read32(Memory::Read32(Memory::Read32(section) + index * 4));
}

// The route point `ahead` links past the ENPT point nearest (x, z).
bool RouteTarget(float x, float z, uint32_t ahead, float& tx, float& tz) {
    const uint32_t manager = Memory::Read32(kKmpManager);
    if (!InGuestRam(manager)) return false;
    const uint32_t points = Memory::Read32(manager + kKmpEnptSection);
    const uint32_t paths = Memory::Read32(manager + kKmpEnphSection);
    if (!InGuestRam(points) || !InGuestRam(paths)) return false;
    const uint32_t count = Memory::Read16(points + 4), groups = Memory::Read16(paths + 4);
    if (count == 0 || groups == 0) return false;
    uint32_t nearest = 0;
    float best = 1e30f;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t raw = Raw(points, i);
        const float dx = Memory::ReadFloat32(raw) - x, dz = Memory::ReadFloat32(raw + 8) - z;
        if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; nearest = i; }
    }
    uint32_t index = nearest;
    for (uint32_t step = 0; step < ahead; ++step) {
        uint32_t group = 0;
        while (group < groups) {
            const uint32_t g = Raw(paths, group);
            if (index >= Memory::Read8(g) && index < uint32_t(Memory::Read8(g)) + Memory::Read8(g + 1)) break;
            ++group;
        }
        if (group == groups) break;
        const uint32_t g = Raw(paths, group);
        if (index + 1 < uint32_t(Memory::Read8(g)) + Memory::Read8(g + 1)) {
            ++index;
        } else {
            const uint32_t next = Memory::Read8(g + 8);  // first next link, 0xFF for none
            index = Memory::Read8(Raw(paths, next < groups ? next : 0));
        }
    }
    const uint32_t raw = Raw(points, index < count ? index : 0);
    tx = Memory::ReadFloat32(raw);
    tz = Memory::ReadFloat32(raw + 8);
    return true;
}

float SteerSign() {
    static const float sign = [] {
        const char* value = std::getenv("MKW100_AUTO_STEER_SIGN");
        return value && value[0] == '-' ? -1.0f : 1.0f;
    }();
    return sign;
}

} // namespace

void AutoDriver::FillPad(PADStatus& status) {
    if (!BootRace::AutoDriven()) {
        return;
    }
    ++g_frame;
    status = PADStatus{};
    status.err = PAD_ERR_NONE;

    // Tests: MKW100_TEST_AWARD_HOLD presses nothing during the award ceremony (game mode 11), so
    // its results page stays up for a screenshot instead of being skipped.
    static const bool holdAward = std::getenv("MKW100_TEST_AWARD_HOLD") != nullptr;
    if (holdAward) {
        const uint32_t racedata = Memory::Read32(0x809BD728);
        if (InGuestRam(racedata) && Memory::Read32(racedata + 0x20 + 0xB50) == 11) return;
    }

    const uint32_t raceinfo = Memory::Read32(kRaceinfoGlobal);
    const uint32_t kartManager = Memory::Read32(kKartManager);
    if (!InGuestRam(raceinfo) || !InGuestRam(kartManager)) {
        // Outside a race: tap A so any screen waiting for a press moves on.
        status.button = (g_frame / 15) % 2 == 0 ? PAD_BUTTON_A : 0;
        return;
    }
    const uint32_t stage = Memory::Read32(raceinfo + kRaceinfoStage);
    if (stage < 2) {
        return;  // countdown: holding A early would burn out
    }
    if (stage > 2) {  // finished: tap A through the results pages
        status.button = (g_frame / 20) % 2 == 0 ? PAD_BUTTON_A : 0;
        return;
    }
    const uint32_t players = Memory::Read32(raceinfo + 0xC);
    const uint32_t player0 = InGuestRam(players) ? Memory::Read32(players) : 0;
    const uint32_t karts = Memory::Read32(kartManager + 0x20);
    const uint32_t kart = InGuestRam(karts) ? Memory::Read32(karts) : 0;
    if (!InGuestRam(player0) || !InGuestRam(kart)) {
        status.button = PAD_BUTTON_A;
        return;
    }

    const uint32_t checkpoint = Memory::Read16(player0 + kRaceinfoPlayerCheckpoint);
    float x = 0, z = 0, fx = 0, fz = 1, tx = 0, tz = 0;
    float steer = 0.0f;
    const bool posed = KartPose(kart, x, z, fx, fz);
    if (posed && g_frame - g_lastProgressFrame >= kStuckFrames) {
        const float mx = x - g_stuckX, mz = z - g_stuckZ;
        if (mx * mx + mz * mz < kStuckDistance * kStuckDistance && g_reverseUntil < g_frame) {
            g_reverseUntil = g_frame + kReverseFrames;
        }
        g_stuckX = x; g_stuckZ = z; g_lastProgressFrame = g_frame;
    }
    if (posed && (CheckpointTarget(checkpoint, kLookAhead, tx, tz) || RouteTarget(x, z, kRouteAhead, tx, tz))) {
        const float dx = tx - x, dz = tz - z;
        const float length = std::sqrt(dx * dx + dz * dz);
        const float flen = std::sqrt(fx * fx + fz * fz);
        if (length > 1.0f && flen > 1e-3f) {
            // Signed angle from forward to the target in the ground plane.
            const float cross = (fx * dz - fz * dx) / (length * flen);
            const float dot = (fx * dx + fz * dz) / (length * flen);
            steer = SteerSign() * std::atan2(cross, dot) * 2.0f;  // full lock at ~30 degrees
        }
    }
    const bool reversing = g_frame < g_reverseUntil;
    if (reversing) {
        steer = -steer;
    }
    steer = std::clamp(steer, -1.0f, 1.0f);
    status.stickX = static_cast<s8>(std::lround(steer * 100.0f));
    status.button = reversing ? PAD_BUTTON_B : PAD_BUTTON_A;
    if (!reversing && g_frame % kItemPeriod < 2) {
        status.button |= PAD_TRIGGER_L;
        status.triggerL = 255;
    }
    if (g_frame % 600 == 0) {
        RT_LOGF("race-auto", "frame %u checkpoint %u pos (%.0f, %.0f) target (%.0f, %.0f) steer %d%s\n", g_frame,
                checkpoint, x, z, tx, tz, int(status.stickX), reversing ? " reversing" : "");
    }
}
