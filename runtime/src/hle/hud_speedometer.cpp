// mkw100: a speedometer in the race HUD, drawn by the game's own layout system (base game).
//
// A port of Pulsar's CtrlRaceSpeedo (MelgMKW/Pulsar, MIT, PulsarEngine/UI/CtrlRaceBase/
// Speedometer.cpp), the speedometer Retro Rewind and other Pulsar packs show. Its layout
// (game_image PULSpeedo.brlyt/.brlan/.brctr) and digit pictures (som_d_*.tpl, speed.tpl) come from
// Pulsar's RaceAssets.szs, repacked on their own into runtime/assets/mkw100/MKW100/MKW100Speedo.szs.
//
//  - Archive: like Pulsar (IO/Misc.cpp), the UI ArchivesHolder gets a third archive (its ctor called
//    with 3 instead of 2 from ArchivesHolder::CreateByType, 0x8052A18C), and when that archive is
//    loaded (ArchiveFile::Load from 0x8052AA2C) its path is our file. The installer puts it in
//    play/mkw100_assets/MKW100/, a disc-shaped overlay root, so the game sees /MKW100/MKW100Speedo.szs.
//  - Controls: Pages::RaceHUD sizes its control group at 0x808562D0 (Page::InitControlGroup); like
//    Pulsar's CustomCtrlBuilder one more slot per local player is added there and a CtrlRaceBase is
//    built in each (LayoutUIControl ctor, CtrlRaceBase vtable, Page::AddControl, ControlLoader::Load
//    with variant Speedo_<players>_<slot>).
//  - Every frame (LayoutUIControl::Update, team_ui.cpp's wrapper calls HudSpeedometerUpdate) the
//    seven digit groups are set to the kart's speed: |engineSpeed + speed2 + speed3|, capped at the
//    hard speed limit, one decimal, as Pulsar computes it.
//
// Base game only: Retro Rewind and Item Fever are Pulsar packs with their own speedometer setting
// and already use that third archive. Off with [local_vs] speedometer = false (F10).

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"
#include "runtime_product.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kSpeedoNew = 0x80229DCC;            // operator new(size), the current heap
constexpr uint32_t kLayoutUIControlCtor = 0x8063D798;
constexpr uint32_t kCtrlRaceBaseVtable = 0x808D3A98;
constexpr uint32_t kCtrlRaceBaseSize = 0x198;
constexpr uint32_t kHudSlot = 0x190;                   // CtrlRaceBase: u8 hudSlotId
constexpr uint32_t kPageAddControl = 0x8060246C;       // Page::AddControl(page, index, control, 0)
constexpr uint32_t kLoaderCtor = 0x805C2C18;           // ControlLoader(this, control)
constexpr uint32_t kLoaderLoad = 0x805C2C60;           // Load(this, folder, ctrl, variant, anims)
constexpr uint32_t kAnimator = 0x98;                   // LayoutUIControl: UIAnimator
constexpr uint32_t kGetAnimationGroup = 0x8063C820;    // UIAnimator::GetAnimationGroupById
constexpr uint32_t kPlayAtFrameAndDisable = 0x8063CB20;

constexpr uint32_t kArchiveMgr = 0x809BD738;           // ArchiveMgr*: ArchivesHolder** at +0x4
constexpr uint32_t kHolderUi = 2;
constexpr uint32_t kHolderArchives = 0x4;              // ArchiveFile* (0x24 each)
constexpr uint32_t kArchiveFileSize = 0x24;

constexpr uint32_t kKartManager = 0x809C18F8;          // Kart::Manager*: Kart::Player** at +0x20
constexpr uint32_t kKartBody = 0x1C + 0x8;             // Player: Pointers at +0x1C; kartBody
constexpr uint32_t kKartMovement = 0x1C + 0x28;
constexpr uint32_t kPhysicsHolder = 0x90;              // Body
constexpr uint32_t kPhysics = 0x4;                     // PhysicsHolder
constexpr uint32_t kSpeed2 = 0xB0, kSpeed3 = 0xC8, kEngineSpeed = 0x14C;
constexpr uint32_t kHardSpeedLimit = 0x2C;             // Movement

constexpr uint32_t kRacedata = 0x809BD728;
constexpr uint32_t kScenarioPlayer = 0x28;             // Racedata + 0x20 + 8
constexpr uint32_t kPlayerLocal = 0x5;                 // s8 local player index
constexpr uint32_t kPlayerType = 0x10;                 // 0 = local human

// Guest strings and scratch for the loader, in the runtime's native table span.
constexpr uint32_t kScratch = ArrayOverflow::kMenuText + 0x4000;
constexpr uint32_t kPathText = kScratch;               // "/MKW100/MKW100Speedo.szs"
constexpr uint32_t kFolderText = kScratch + 0x40;      // "game_image"
constexpr uint32_t kCtrlText = kScratch + 0x60;        // "PULSpeedo"
constexpr uint32_t kVariantText = kScratch + 0x80;     // "Speedo_n_m"
constexpr uint32_t kAnimNames = kScratch + 0xA0;       // the 7 group names
constexpr uint32_t kAnimTable = kScratch + 0x180;      // {name, name, null} x 7, null
constexpr uint32_t kLoader = kScratch + 0x200;         // ControlLoader (small)
constexpr uint32_t kMaxSpeedos = 4;

std::array<uint32_t, kMaxSpeedos> gSpeedos{};          // our controls in the current race HUD

bool SpeedoProduct() { return !RuntimeProduct::IsRetroRewind(); }

void WriteText(uint32_t at, const char* text) {
    for (size_t i = 0; i <= std::strlen(text); ++i) Memory::Write8(at + uint32_t(i), uint8_t(text[i]));
}

void WriteStrings() {
    static bool written = false;
    if (written) return;
    written = true;
    WriteText(kPathText, "/MKW100/MKW100Speedo.szs");
    WriteText(kFolderText, "game_image");
    WriteText(kCtrlText, "PULSpeedo");
    static const char* const kGroups[7] = {"Hundreds", "Tens", "Units", "Dot", "Tenths", "Hundredths", "Thousandths"};
    uint32_t name = kAnimNames;
    for (uint32_t g = 0; g < 7; ++g) {
        WriteText(name, kGroups[g]);
        Memory::Write32(kAnimTable + g * 12 + 0, name);  // group name
        Memory::Write32(kAnimTable + g * 12 + 4, name);  // its one animation, same name
        Memory::Write32(kAnimTable + g * 12 + 8, 0);
        name += 0x18;
    }
    Memory::Write32(kAnimTable + 7 * 12, 0);
}

uint32_t Call(CpuContext* ctx, const CpuContext& saved, uint32_t fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0,
              uint32_t r6 = 0, uint32_t r7 = 0) {
    *ctx = saved;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5; ctx->gpr[6] = r6; ctx->gpr[7] = r7;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

bool InRam(uint32_t a) { return (a >= 0x80000000u && a < 0x81800000u) || (a >= 0x90000000u && a < 0x98000000u); }

// The racer the hud slot belongs to: the local human whose local index is the slot.
uint32_t RacerOfSlot(uint32_t slot) {
    const uint32_t rd = Memory::Read32(kRacedata);
    if (rd == 0) return 0xFFFFFFFF;
    const uint32_t count = Memory::Read8(rd + 0x24);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t p = rd + MKW100_PlayerSlotOffset(i) + kScenarioPlayer;
        if (Memory::Read32(p + kPlayerType) == 0 && int8_t(Memory::Read8(p + kPlayerLocal)) == int32_t(slot)) return i;
    }
    return slot == 0 ? 0 : 0xFFFFFFFF;
}

float KartSpeed(uint32_t racer) {
    const uint32_t manager = Memory::Read32(kKartManager);
    if (!InRam(manager) || racer >= Memory::Read8(manager + 0x24)) return 0.0f;
    const uint32_t player = Memory::Read32(Memory::Read32(manager + 0x20) + 4 * racer);
    if (!InRam(player)) return 0.0f;
    const uint32_t body = Memory::Read32(player + kKartBody);
    const uint32_t movement = Memory::Read32(player + kKartMovement);
    if (!InRam(body) || !InRam(movement)) return 0.0f;
    const uint32_t holder = Memory::Read32(body + kPhysicsHolder);
    const uint32_t physics = InRam(holder) ? Memory::Read32(holder + kPhysics) : 0;
    if (!InRam(physics)) return 0.0f;
    float v[3];
    for (uint32_t k = 0; k < 3; ++k) {
        v[k] = Memory::ReadFloat32(physics + kEngineSpeed + 4 * k) + Memory::ReadFloat32(physics + kSpeed2 + 4 * k) +
               Memory::ReadFloat32(physics + kSpeed3 + 4 * k);
    }
    float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    const float cap = Memory::ReadFloat32(movement + kHardSpeedLimit);
    if (cap > 0.0f && speed > cap) speed = cap;
    return std::isfinite(speed) ? speed : 0.0f;
}

void Animate(CpuContext* ctx, const CpuContext& saved, uint32_t control, const uint32_t (&frames)[7]) {
    for (uint32_t g = 0; g < 7; ++g) {
        const uint32_t group = Call(ctx, saved, kGetAnimationGroup, control + kAnimator, g);
        if (!InRam(group)) continue;
        *ctx = saved;
        ctx->gpr[3] = group;
        ctx->gpr[4] = 0;
        ctx->fpr[1].d = static_cast<double>(frames[g]);
        InvokeIndirectCpu(kPlayAtFrameAndDisable, ctx);
    }
    *ctx = saved;
}

} // namespace

// Called from team_ui.cpp's LayoutUIControl::Update wrapper for every control.
void HudSpeedometerUpdate(CpuContext* ctx, uint32_t control) {
    if (control == 0) return;
    uint32_t slot = kMaxSpeedos;
    for (uint32_t i = 0; i < kMaxSpeedos; ++i) if (gSpeedos[i] == control) slot = i;
    if (slot == kMaxSpeedos) return;
    const CpuContext saved = *ctx;
    // 10 shows nothing, 11 the dot. One decimal; below 100 the digits move left, as Pulsar does.
    const uint32_t value = static_cast<uint32_t>(KartSpeed(RacerOfSlot(Memory::Read8(control + kHudSlot))) * 1000.0f);
    uint32_t hundreds = value % 1000000 / 100000, tens = value % 100000 / 10000, units = value % 10000 / 1000;
    uint32_t dot = 11, tenths = value % 1000 / 100, hundredths = 10, thousandths = 10;
    if (value < 10000) {
        hundreds = units; tens = dot; units = tenths; dot = hundredths; tenths = thousandths; hundredths = 10; thousandths = 10;
    } else if (value < 100000) {
        hundreds = tens; tens = units; units = dot; dot = tenths; tenths = hundredths; hundredths = thousandths; thousandths = 10;
    }
    const uint32_t frames[7] = {hundreds, tens, units, dot, tenths, hundredths, thousandths};
    Animate(ctx, saved, control, frames);
}

namespace {
// The overlay root holding /MKW100/MKW100Speedo.szs (next to the executable).
struct RegisterSpeedoAssets {
    RegisterSpeedoAssets() { RecompMod::RegisterDvdOverlayRoot("mkw100_assets"); }
} gRegisterSpeedoAssets;
} // namespace

// ArchivesHolder::ArchivesHolder(this, u16 archiveCount): the UI holder gets a third archive.
extern "C" void HudSpeedo_ArchivesHolderCtor_8052a538(CpuContext* ctx) {
    if (ctx->lr == 0x8052A190 && (ctx->gpr[4] & 0xFFFF) == 2 && SpeedoProduct()) ctx->gpr[4] = 3;
    MKW_GUEST_NEXT(8052A538)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8052A538, HudSpeedo_ArchivesHolderCtor_8052a538, "HudSpeedo_ArchivesHolderCtor_8052a538");

// ArchiveFile::Load(this, path, ...): the UI holder's third archive is our file.
extern "C" void HudSpeedo_ArchiveFileLoad_80518e10(CpuContext* ctx) {
    if (ctx->lr == 0x8052AA30 && SpeedoProduct()) {
        const uint32_t mgr = Memory::Read32(kArchiveMgr);
        const uint32_t holders = InRam(mgr) ? Memory::Read32(mgr + 0x4) : 0;
        const uint32_t ui = InRam(holders) ? Memory::Read32(holders + 4 * kHolderUi) : 0;
        const uint32_t archives = InRam(ui) ? Memory::Read32(ui + kHolderArchives) : 0;
        if (archives != 0 && ctx->gpr[3] == archives + 2 * kArchiveFileSize) {
            WriteStrings();
            ctx->gpr[4] = kPathText;
        }
    }
    MKW_GUEST_NEXT(80518E10)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80518E10, HudSpeedo_ArchiveFileLoad_80518e10, "HudSpeedo_ArchiveFileLoad_80518e10");

// Page::InitControlGroup(page, count) from Pages::RaceHUD::OnInit: room for the speedometers, then
// build them in the extra slots.
extern "C" void HudSpeedo_InitControlGroup_8060245c(CpuContext* ctx) {
    const bool raceHud = ctx->lr == 0x808562D4;
    if (!raceHud) {
        MKW_GUEST_NEXT(8060245C)(ctx);
        return;
    }
    gSpeedos.fill(0);
    const uint32_t page = ctx->gpr[3];
    const uint32_t base = ctx->gpr[4];
    const uint32_t players = std::min<uint32_t>(std::max<uint32_t>(LocalVs::LocalPlayers(), 1), kMaxSpeedos);
    const uint32_t extra = SpeedoProduct() && LocalVs::SpeedometerEnabled() ? players : 0;
    ctx->gpr[4] = base + extra;
    MKW_GUEST_NEXT(8060245C)(ctx);
    if (extra == 0) return;
    WriteStrings();
    const CpuContext saved = *ctx;
    const uint32_t type = players == 3 ? 4 : players;
    for (uint32_t i = 0; i < extra; ++i) {
        const uint32_t control = Call(ctx, saved, kSpeedoNew, kCtrlRaceBaseSize);
        if (!InRam(control)) break;
        Call(ctx, saved, kLayoutUIControlCtor, control);
        Memory::Write32(control, kCtrlRaceBaseVtable);
        for (uint32_t o = 0x174; o < kCtrlRaceBaseSize; o += 4) Memory::Write32(control + o, 0);
        Memory::Write8(control + kHudSlot, uint8_t(i));
        Call(ctx, saved, kPageAddControl, page, base + i, control, 0);
        char variant[16];
        std::snprintf(variant, sizeof variant, "Speedo_%u_%u", type, i);
        WriteText(kVariantText, variant);
        Call(ctx, saved, kLoaderCtor, kLoader, control);
        Call(ctx, saved, kLoaderLoad, kLoader, kFolderText, kCtrlText, kVariantText, kAnimTable);
        gSpeedos[i] = control;
        const uint32_t zero[7] = {0, 0, 0, 0, 0, 0, 0};
        Animate(ctx, saved, control, zero);
    }
    *ctx = saved;
    RT_LOGF("speedometer", "race HUD: %u speedometer(s) added\n", extra);
}
REGISTER_NATIVE_WRAPPER_AS(0x8060245C, HudSpeedo_InitControlGroup_8060245c, "HudSpeedo_InitControlGroup_8060245c");

bool HudSpeedometerNative() { return SpeedoProduct() && gSpeedos[0] != 0; }
