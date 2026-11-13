// mkw100: 200cc with brake drifting, in place of Mirror mode.
//
// With kMirrorIs200cc on, a GP or VS race the menus set up as 150cc Mirror runs unmirrored at
// 200cc instead. The values follow Pulsar's 200cc (MIT, MelgMKW; Race/200cc.cpp,
// Race/LapSpeedModifier.cpp, Race/200ccParams.hpp), reimplemented here as native wrappers:
//
//  - Racedata::InitRace (0x805302C4, wrapped in player_slots.cpp): after it copies the menu scenario, a 150cc GP/VS race with
//    the mirror flag (settings modeFlags bit 1) has the flag cleared and is marked 200cc.
//  - Kart::ComputeStats (0x80591FA4) from its call in the kart setup (0x8058F670): baseSpeed and
//    the 4 acceleration stages x1.5, the speed globals (speed cap, shells, star, bullet, mega,
//    boost acceleration) x5/3. Pulsar runs 200cc on the 100cc slot (speed ratio 0.9, table at
//    0x808B5CD0: 3.0 0.8 0.9 1.0 0.7) and scales stats by 5/3, 1.5 overall; this runs on the
//    150cc slot (ratio 1.0), so the stat factor is 1.5 and the globals keep Pulsar's 5/3.
//  - Movement::UpdateKartSpeed (vtable +0x44, 0x8057AB68 in all three Movement vtables) from its
//    call in Movement::Update (0x80579910): while brake drifting the acceleration (+0x30) is
//    -1.5. Brake drifting: drifting (Status bitfield0 0x40000, low bits 0xF, none of
//    0x80100000) with accelerate, brake and drift held. Pulsar sets an input action bit in a
//    frame hook and tests it; the raw buttons are read here directly, same masks per controller.
//  - Status::UpdateFromInput (0x8059487C) from Kart::Sub (0x805967A4): fast falling, airborne
//    gravity -= 2 * stick up * 0.39.
//
//  - Kart::Movement::UpdateCannon (0x80584D58): each flight frame the game sets engineSpeed (+0x20)
//    to baseSpeed (+0x14) and then lowers it to the cannon's deceleration curve near the end;
//    Pulsar starts from baseSpeed x2/3 instead (its store at 0x805850C8). As min(base, curve) is
//    what the game leaves, min(that, base x2/3) afterwards gives Pulsar's value on every path.
//
//  - WheelPhysicsHolder::Update (0x8059A278) from Kart::Player::ApplyPhysics (0x805973B4): the
//    gravity vector's y gets Pulsar's wheel fast falling (x (1 + 2 * stick up * 0.3)); on a kart's
//    first airborne frame status +0x96 keeps whether bitfield0 0x80 was set, which both fast
//    fallings read (Pulsar's FastFallingWheels at 0x805973A4).
//  - Kart::Link::GetDriftState (0x805911D8) from Effects::Player::UpdateKartEffects (0x8069804C)
//    and UpdateBikeEffects (0x80698F8C): while brake drifting the brake-drift sparks are created
//    and updated (kart drift effects 34..35, bike 25), else faded (Pulsar's BrakeEffectKarts/Bikes).
//  - The engine sound: a loadhook site (array-sites.txt 0x806FAFEC) reads the status flags as 2
//    while brake drifting (Pulsar's BrakeDriftingSoundWrapper); the state per kart comes from
//    the UpdateKartSpeed wrapper below, the same frame.
//
// Not ported: the ghost writer fix (TT has no mirror).

#include "abi_bridge.h"
#include "hle/cc200.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <unordered_map>

namespace {

// Off: Mirror is vanilla Mirror.
constexpr bool kMirrorIs200cc = true;

constexpr uint32_t kCcRacedataGlobal = 0x809BD728;   // RaceConfig::spInstance
constexpr uint32_t kCcRacesSettings = 0x20 + 0xB48;  // races scenario settings
constexpr uint32_t kCcEngineClass = 0x4;
constexpr uint32_t kCcGameMode = 0x8;
constexpr uint32_t kCcModeFlags = 0x28;
constexpr uint32_t kCcMirrorFlag = 1;
constexpr uint32_t kCcCc150 = 2;
constexpr uint32_t kCcModeVs = 1;                    // 0 GP, 1 VS

constexpr float kCcStatFactor = 1.5f;                // 0.9 * 5/3 over the 150cc ratio 1.0
constexpr float kCcGlobalFactor = 5.0f / 3.0f;       // Pulsar's speedFactor
constexpr float kCcBrakeDriftAccel = -1.5f;
constexpr float kCcFastFallGravity = 0.39f;
constexpr float kCcCannonExit = 2.0f / 3.0f;         // Pulsar's cannonExit
constexpr float kCcFastFallWheelGravity = 0.3f;     // Pulsar's fastFallingWheelGravity
constexpr uint32_t kCcCreateEffects = 0x806A19D8;   // Effects::Player::CreateAndUpdateEffectsByIdxVelocity
constexpr uint32_t kCcFadeEffects = 0x806A1B68;     // Effects::Player::FollowFadeEffectsByIdxVelocity

// Brake drifting per kart Status this frame (UpdateKartSpeed), for the sparks and the sound.
std::unordered_map<uint32_t, bool> g_brakeDrifting;
bool BrakeDriftingNow(uint32_t status) {
    const auto it = g_brakeDrifting.find(status);
    return it != g_brakeDrifting.end() && it->second;
}

struct Cc200SpeedGlobal { uint32_t addr; float vanilla; };
constexpr Cc200SpeedGlobal kCcSpeedGlobals[] = {
    {0x808A5DC0, 105.0f},    // green shell
    {0x808A5AB0, 75.0f},     // red shell initial
    {0x808A5ABC, 130.0f},    // red shell
    {0x808A5BC4, 260.0f},    // blue shell
    {0x808A5C24, 130.0f},    // blue shell homing
    {0x808A5C34, 640000.0f}, // blue shell minimum dive distance
    {0x808B59F4, 120.0f},    // hard speed cap
    {0x808B59F8, 145.0f},    // bullet
    {0x808B5CAC, 105.0f},    // star
    {0x808B5CBC, 95.0f},     // mega
    {0x808CB550, 70.0f},     // unknown_70
    {0x808B5C78, 3.0f},      // regular boost acceleration
};
constexpr uint32_t kCcMinDriftSpeedRatio = 0x808B5B1C;  // 0.55, divided by the factor

bool g_ccis200 = false;
bool g_ccglobalsScaled = false;

void Cc200ApplyGlobals(bool scaled) {
    const float f = scaled ? kCcGlobalFactor : 1.0f;
    for (const Cc200SpeedGlobal& g : kCcSpeedGlobals) Memory::WriteFloat32(g.addr, g.vanilla * f);
    Memory::WriteFloat32(kCcMinDriftSpeedRatio, 0.55f / f);
    g_ccglobalsScaled = scaled;
}

uint32_t Cc200CallGuest(CpuContext* ctx, uint32_t function, uint32_t r3) {
    ctx->gpr[3] = r3;
    InvokeIndirectCpu(function, ctx);
    return ctx->gpr[3];
}

// ControllerHolder for a kart Status: Link::GetControllerHolder(status->link).
uint32_t Cc200ControllerHolderOf(CpuContext* ctx, uint32_t status) {
    return Cc200CallGuest(ctx, 0x805903F4, Memory::Read32(status + 0x18));
}

bool Cc200IsBrakeDrifting(CpuContext* ctx, uint32_t status) {
    const uint32_t bits = Memory::Read32(status + 0x4);
    if ((bits & 0x40000) == 0 || (bits & 0x1F) != 0xF || (bits & 0x80100000) != 0) return false;
    const uint32_t holder = Cc200ControllerHolderOf(ctx, status);
    if (holder == 0) return false;
    const uint32_t controller = Memory::Read32(holder + 0x4);
    uint32_t mask = 0x700;  // Wii wheel / Wiimote
    if (controller != 0) {
        const uint32_t getType = Memory::Read32(Memory::Read32(controller) + 0x10);
        switch (int32_t(Cc200CallGuest(ctx, getType, controller))) {
        case 1: mask = 0xC04; break;  // nunchuck
        case 2: mask = 0x250; break;  // classic
        case 3: mask = 0x320; break;  // GameCube
        default: break;
        }
    }
    const uint32_t raw = Memory::Read16(holder + 0x28 + 0x6);  // inputStates[0].buttonRaw
    return (raw & mask) == mask;
}

} // namespace

namespace Cc200 {

void OnInitRace(uint32_t racedata) {
    g_ccis200 = false;
    if (kMirrorIs200cc && racedata != 0) {
        const uint32_t s = racedata + kCcRacesSettings;
        const uint32_t flags = Memory::Read32(s + kCcModeFlags);
        if ((flags & kCcMirrorFlag) && Memory::Read32(s + kCcEngineClass) == kCcCc150 &&
            Memory::Read32(s + kCcGameMode) <= kCcModeVs) {
            Memory::Write32(s + kCcModeFlags, flags & ~kCcMirrorFlag);
            g_ccis200 = true;
        }
    }
    if (g_ccis200 != g_ccglobalsScaled) Cc200ApplyGlobals(g_ccis200);
}

bool Active() { return g_ccis200; }

bool ReplacesMirror() { return kMirrorIs200cc; }

} // namespace Cc200

extern "C" void Cc200_ComputeStats_80591fa4(CpuContext* ctx) {
    const bool site = ctx->lr == 0x8058F674;
    MKW_GUEST_NEXT(80591FA4)(ctx);
    const uint32_t stats = ctx->gpr[3];
    if (!g_ccis200 || !site || stats == 0) return;
    for (uint32_t off = 0x18; off <= 0x30; off += 4) {  // baseSpeed, then as[0..3] from 0x24
        if (off == 0x1C || off == 0x20) continue;       // handling multiplier, tilt
        Memory::WriteFloat32(stats + off, Memory::ReadFloat32(stats + off) * kCcStatFactor);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80591FA4, Cc200_ComputeStats_80591fa4, "Cc200_ComputeStats_80591fa4");

extern "C" void Cc200_UpdateKartSpeed_8057ab68(CpuContext* ctx) {
    const uint32_t movement = ctx->gpr[3];
    const uint32_t lr = ctx->lr;
    MKW_GUEST_NEXT(8057AB68)(ctx);
    if (!g_ccis200 || lr != 0x80579914) return;
    const uint32_t ret = ctx->gpr[3];
    const uint32_t status = Memory::Read32(Memory::Read32(movement) + 0x4);  // pointers->kartStatus
    const bool braking = status != 0 && Cc200IsBrakeDrifting(ctx, status);
    if (status != 0) g_brakeDrifting[status] = braking;
    if (braking) Memory::WriteFloat32(movement + 0x30, kCcBrakeDriftAccel);
    ctx->gpr[3] = ret;
    ctx->lr = lr;
}
REGISTER_NATIVE_WRAPPER_AS(0x8057AB68, Cc200_UpdateKartSpeed_8057ab68, "Cc200_UpdateKartSpeed_8057ab68");

extern "C" void Cc200_StatusUpdateFromInput_8059487c(CpuContext* ctx) {
    if (g_ccis200 && ctx->lr == 0x805967A8) {
        const uint32_t status = ctx->gpr[3];
        const uint32_t physics = ctx->gpr[4];
        const uint32_t airtime = Memory::Read32(status + 0x1C);
        const bool flag96 = Memory::Read8(status + 0x96) != 0;
        if (airtime >= 2 && (!flag96 || airtime > 19)) {
            const uint32_t lr = ctx->lr;
            const uint32_t holder = Cc200ControllerHolderOf(ctx, status);
            ctx->gpr[3] = status;
            ctx->lr = lr;
            if (holder != 0) {
                const float up = Memory::ReadFloat32(holder + 0x28 + 0xC);  // inputStates[0].stick y
                if (up > 0.0f) {
                    Memory::WriteFloat32(physics + 0x148,
                                         Memory::ReadFloat32(physics + 0x148) - 2.0f * up * kCcFastFallGravity);
                }
            }
        }
    }
    MKW_GUEST_NEXT(8059487C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8059487C, Cc200_StatusUpdateFromInput_8059487c, "Cc200_StatusUpdateFromInput_8059487c");

// Cannon flight in 200cc: Pulsar's exit speed (see the header).
extern "C" void Cc200_UpdateCannon_80584d58(CpuContext* ctx) {
    const uint32_t movement = ctx->gpr[3];
    MKW_GUEST_NEXT(80584D58)(ctx);
    if (!g_ccis200 || movement == 0) return;
    const float cap = Memory::ReadFloat32(movement + 0x14) * kCcCannonExit;
    if (Memory::ReadFloat32(movement + 0x20) > cap) Memory::WriteFloat32(movement + 0x20, cap);
}
REGISTER_NATIVE_WRAPPER_AS(0x80584D58, Cc200_UpdateCannon_80584d58, "Cc200_UpdateCannon_80584d58");

// 200cc wheel fast falling (see the header).
extern "C" void Cc200_WheelPhysicsUpdate_8059a278(CpuContext* ctx) {
    if (g_ccis200 && ctx->lr == 0x805973B8) {
        const uint32_t kart = ctx->gpr[28];                        // Kart::Player (a Link)
        const uint32_t status = Memory::Read32(Memory::Read32(kart) + 0x4);
        const uint32_t gravity = ctx->gpr[4];                      // Vec3 on the caller's stack
        if (status != 0) {
            const uint32_t airtime = Memory::Read32(status + 0x1C);
            if (airtime == 0) {
                Memory::Write8(status + 0x96, (Memory::Read32(status + 0x4) & 0x80) != 0 ? 1 : 0);
            } else if (airtime >= 2 && (Memory::Read8(status + 0x96) == 0 || airtime > 19)) {
                const CpuContext saved = *ctx;
                const uint32_t holder = Cc200ControllerHolderOf(ctx, status);
                *ctx = saved;
                if (holder != 0) {
                    const float up = Memory::ReadFloat32(holder + 0x28 + 0xC);  // inputStates[0].stick y
                    const float input = up > 0.0f ? 2.0f * up : 0.0f;
                    Memory::WriteFloat32(gravity + 4,
                                         Memory::ReadFloat32(gravity + 4) * (input * kCcFastFallWheelGravity + 1.0f));
                }
            }
        }
    }
    MKW_GUEST_NEXT(8059A278)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8059A278, Cc200_WheelPhysicsUpdate_8059a278, "Cc200_WheelPhysicsUpdate_8059a278");

// 200cc brake-drift sparks (see the header): Effects::Player is r31 (karts) or r30 (bikes) at the
// GetDriftState call, which goes on to return the drift state as before.
extern "C" void Cc200_GetDriftState_805911d8(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    if (g_ccis200 && (lr == 0x80698050 || lr == 0x80698F90)) {
        const bool kartEffects = lr == 0x80698050;
        const uint32_t effects = ctx->gpr[kartEffects ? 31 : 30];
        const uint32_t player = ctx->gpr[3];
        const uint32_t status = player ? Memory::Read32(Memory::Read32(player) + 0x4) : 0;
        const uint32_t array = Memory::Read32(effects + (kartEffects ? 0x530 : 0x614));
        const uint32_t first = kartEffects ? 34 : 25, end = kartEffects ? 36 : 26;
        bool present = array != 0;
        for (uint32_t i = first; present && i < end; ++i) present = Memory::Read32(array + 4 * i) != 0;
        if (status != 0 && present) {
            const CpuContext saved = *ctx;
            ctx->gpr[3] = effects;
            ctx->gpr[4] = array;
            ctx->gpr[5] = first;
            ctx->gpr[6] = end;
            ctx->gpr[7] = 1;
            InvokeIndirectCpu(BrakeDriftingNow(status) ? kCcCreateEffects : kCcFadeEffects, ctx);
            *ctx = saved;
        }
    }
    MKW_GUEST_NEXT(805911D8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805911D8, Cc200_GetDriftState_805911d8, "Cc200_GetDriftState_805911d8");

// loadhook sites (array-sites.txt). 1: Audio::EngineMgr::Update's status flags read as 2 while that
// kart brake drifts in 200cc, as Pulsar's BrakeDriftingSoundWrapper.
extern "C" uint32_t MKW100_LoadHook(uint32_t base, int32_t disp, uint32_t id) {
    const uint32_t word = Memory::Read32(base + uint32_t(disp));
    if (id == 1 && g_ccis200 && disp == 0x4 && BrakeDriftingNow(base)) return 2;
    return word;
}
