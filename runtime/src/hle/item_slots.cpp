// mkw100: item probabilities for races of more than 12 players.
//
// ItemSlot.bin holds one row of item weights per race position, 12 rows (0x26 bytes: 19 items x
// u16). The item table object (global 0x809C3670) loads it into `new u16[rows][19]` tables
// (0x807BA9D8; {rows, table*} pairs at +0x10 and +0x18), then 0x807BAD20 remaps each table in
// place to one row per player: row i = file row map[playerCount][i] - 1, from a static map at
// 0x808A6338 that covers player counts 1..12 only. With more players it reads past the map and
// writes rows 12.. past the 12-row allocation (it broke the next heap block at 24 players).
//
// For more than 12 players the remap is done here instead: the table becomes a game-allocated
// `count`-row table, and position i (0-based) gets the file row LocalVs::ItemRowFor picks for the
// "item_distribution" setting. The default ("even") is i * rows / count, so the 12 rows are spread
// evenly over the field (positions 1-2 of 24 share row 1, 23-24 share row 12). A gameplay choice,
// not a reconstruction: the game has no data for 13+ players. Up to 12 players the game's own remap
// runs unchanged.

#include "abi_bridge.h"
#include "local_vs.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

constexpr uint32_t kItemMapPlayers = 12;  // the static remap covers 1..12 players
constexpr uint32_t kPlayerCountOffset = 0x44;  // u32, the item table object's player count
constexpr uint32_t kRowBytes = 0x26;
constexpr uint32_t kNewArray = 0x80229DF0;     // operator new[](size), the current heap
constexpr uint32_t kDeleteArray = 0x80229EE0;  // operator delete[](void*)

uint32_t CallGuest(CpuContext* ctx, uint32_t function, uint32_t argument) {
    ctx->gpr[3] = argument;
    InvokeIndirectCpu(function, ctx);
    return ctx->gpr[3];
}

} // namespace

// void ItemSlotTable::remap(ItemSlotTable* this, {u32 rows; u16* table;}* table)
extern "C" void ItemSlotTable_Remap_807bad20(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t table = ctx->gpr[4];
    const uint32_t count = Memory::Read32(self + kPlayerCountOffset);
    if (count <= kItemMapPlayers) {
        MKW_GUEST_NEXT(807BAD20)(ctx);
        return;
    }
    const uint32_t rows = Memory::Read32(table);
    const uint32_t source = Memory::Read32(table + 4);
    if (rows == 0 || source == 0) {
        throw std::runtime_error("mkw100 item slots: empty item table");
    }
    std::vector<uint8_t> file(rows * kRowBytes);
    for (uint32_t i = 0; i < file.size(); ++i) {
        file[i] = Memory::Read8(source + i);
    }

    const CpuContext saved = *ctx;
    const uint32_t remapped = CallGuest(ctx, kNewArray, count * kRowBytes);
    if (remapped == 0) {
        throw std::runtime_error("mkw100 item slots: out of memory for the item table");
    }
    for (uint32_t position = 0; position < count; ++position) {
        const uint32_t row = LocalVs::ItemRowFor(position, count, rows);
        for (uint32_t i = 0; i < kRowBytes; ++i) {
            Memory::Write8(remapped + position * kRowBytes + i, file[row * kRowBytes + i]);
        }
    }
    Memory::Write32(table + 4, remapped);
    CallGuest(ctx, kDeleteArray, source);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807BAD20, ItemSlotTable_Remap_807bad20, "ItemSlotTable_Remap_807bad20");

// Retro Rewind overlays ItemSlotTable::DecideRouletteItem (0x807BB8D0): it reads the roulette list
// of the racer's position from a 12-row table (+0x2C, 0x50 per row) that its own build code makes,
// so position 13+ read past it (24 racers: a garbage list, then a garbage item count crashed).
// Under a mod overlay, past 12 players the position is mapped to the row the item tables use for
// it. The base game's own function is left alone.
extern "C" void ItemSlotTable_DecideRouletteItem_807bb8d0(CpuContext* ctx) {
    static const bool overlaid = GuestFunctions::NextTranslation(MKW_GADDR(807BB8D0)) !=
                                 GuestFunctions::BaseTranslation(MKW_GADDR(807BB8D0));
    const uint32_t count = Memory::Read32(ctx->gpr[3] + kPlayerCountOffset);
    const uint32_t position = ctx->gpr[5];
    if (overlaid && count > kItemMapPlayers && position > kItemMapPlayers && position <= count) {
        ctx->gpr[5] = LocalVs::ItemRowFor(position - 1, count, kItemMapPlayers) + 1;
    }
    MKW_GUEST_NEXT(807BB8D0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807BB8D0, ItemSlotTable_DecideRouletteItem_807bb8d0,
                           "ItemSlotTable_DecideRouletteItem_807bb8d0");

// Item::SlotData::Init (0x807BAF48) loads the player and CPU item tables (+0x10, +0x18) three ways:
// online (0x809C38B9 set) and offline with 0x809C38BA set (VS, battle) remap them to one row per
// player (0x807BAD20 above); offline with 0x809C38BA clear (Grand Prix) leaves them as the file's
// 12 rows, so past 12 players a position read past the table (100 racers, last place: green
// shells). There the remap is run after Init, as the other paths do.
extern "C" void ItemSlotData_Init_807baf48(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    MKW_GUEST_NEXT(807BAF48)(ctx);
    if (Memory::Read32(self + kPlayerCountOffset) <= kItemMapPlayers) return;
    if (Memory::Read8(0x809C38B9) != 0 || Memory::Read8(0x809C38BA) != 0) return;  // already remapped
    const CpuContext saved = *ctx;
    for (const uint32_t table : {0x10u, 0x18u}) {
        *ctx = saved;
        ctx->gpr[3] = self;
        ctx->gpr[4] = self + table;
        InvokeIndirectCpu(MKW_GADDR(807BAD20), ctx);
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807BAF48, ItemSlotData_Init_807baf48, "ItemSlotData_Init_807baf48");
