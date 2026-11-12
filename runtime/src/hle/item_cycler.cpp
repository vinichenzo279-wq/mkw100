// mkw100: debug item cycler for testing, a native port of "Item Cycler (Debug and Infinite)" by _Ro
// (https://mariokartwii.com/showthread.php?tid=2674, a Gecko code hooking Item::Player::Update at
// PAL 0x80797DD8; source https://github.com/RoGamer97/My-Cheat-Code-Collection_ASM-Hacks).
//
// Config.toml [local_vs] item_cycler = "off" (default), "debug" or "infinite"; also in F10. Offline
// races only. For each local racer, Y + D-pad left/right (Classic/GameCube; D-pad alone on a
// Nunchuk) steps through Empty, Green Shell, Red Shell, Banana, Fake Box, Mushroom, Triple
// Mushroom, Bob-omb, Blue Shell, Lightning, Star, Golden Mushroom, Mega, Blooper, POW, Thunder
// Cloud, Bullet Bill, Triple Green, Triple Red, Triple Banana and puts the item in the slot. Under
// Item Fever the cycle goes on through its own items, ids 0x15..0x29 (its code treats exactly that
// range as its items, e.g. 0x81855C68).
//   debug:    items are used up as normal; using an item with the slot empty refills it with the
//             last cycled item.
//   infinite: the slot keeps the cycled item (it is put back whenever it changes).
// Nothing happens while the roulette spins, with a Wii Wheel, or for CPU and online racers.
// The state (the cycled index) is kept in Item::Player's padding byte 0xB3, as in the code.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "memory.h"
#include "recomp_mod_loader.h"

#include <array>
#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kGetControllerHolder = 0x805903F4;  // Kart::Link::GetControllerHolder(this)
constexpr uint32_t kSetItem = 0x807BC940;              // Item::PlayerInventory::SetItem(inv, item, 0)
constexpr uint32_t kCycleIndex = 0xB3;                 // u8 padding in Item::Player
constexpr uint32_t kRoulette = 0x58;                   // nonzero while the slot spins
constexpr uint32_t kUseFlags = 0xC;                    // u16
constexpr uint32_t kInventory = 0x88;
constexpr uint32_t kSlotItem = 0x8C;                   // the item in the slot (0x14 = none)
constexpr uint32_t kControllerType = 0xC8;             // ControllerHolder, 0 = Wii Wheel
constexpr uint32_t kButtons = 0x2C;                    // inputStates[0] actions << 16 | raw
constexpr uint32_t kPreviousButtons = 0x44;            // inputStates[1]
constexpr uint32_t kNetController = 0x809C20D8;        // RKNet::Controller*: roomType at +0xE8

// Item ids in cycle order; index 0 (0x14) is the empty slot.
constexpr std::array<uint8_t, 41> kCycleIds{{0x14, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                             0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
                                             0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
                                             0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29}};
constexpr uint32_t kStockCycle = 20;  // empty + the 19 stock items

// The ids in the cycle: Item Fever's 21 items too when that pack runs.
uint32_t CycleSize() {
    static const bool itemFever = RecompMod::RiivolutionXml().find("ItemFever") != std::string::npos;
    return itemFever ? uint32_t(kCycleIds.size()) : kStockCycle;
}

bool Offline() {
    const uint32_t net = Memory::Read32(kNetController);
    return net == 0 || Memory::Read32(net + 0xE8) == 0;
}

void SetSlot(CpuContext* ctx, const CpuContext& saved, uint32_t player, uint32_t item) {
    *ctx = saved;
    ctx->gpr[3] = player + kInventory;
    ctx->gpr[4] = item;
    ctx->gpr[5] = 0;
    InvokeIndirectCpu(kSetItem, ctx);
}

} // namespace

// Item::Player::Update(this)
extern "C" void ItemCycler_PlayerUpdate_80797928(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[3];
    MKW_GUEST_NEXT(80797928)(ctx);
    const LocalVs::ItemCycler mode = LocalVs::CurrentItemCycler();
    if (mode == LocalVs::ItemCycler::Off || !Offline()) return;
    // Only local racers: Kart::Link -> pointers -> flags at +0x14, bit 1.
    const uint32_t link = Memory::Read32(player);
    if (link < 0x80000000u) return;
    const uint32_t pointers = Memory::Read32(link + 4);
    if (pointers < 0x80000000u || (Memory::Read32(pointers + 0x14) & 2) == 0) return;

    const CpuContext saved = *ctx;
    ctx->gpr[3] = player;
    InvokeIndirectCpu(kGetControllerHolder, ctx);
    const uint32_t holder = ctx->gpr[3];
    *ctx = saved;
    if (holder < 0x80000000u) return;
    const uint32_t type = Memory::Read32(holder + kControllerType);
    if (type == 0 || Memory::Read32(player + kRoulette) != 0) return;

    uint32_t index = Memory::Read8(player + kCycleIndex);
    const uint32_t size = CycleSize();
    if (index >= size) index = 0;
    const uint32_t buttons = Memory::Read32(holder + kButtons);
    const uint32_t previous = Memory::Read32(holder + kPreviousButtons);
    const uint32_t hold = type < 2 ? type : type == 2 ? 0x20 : 0x800;  // Nunchuk, Classic Y, GameCube Y
    const uint32_t pressed = buttons & ~previous & 3;                  // D-pad left (1) / right (2)
    if ((buttons & hold) != 0 && pressed != 0) {
        index = (pressed & 1) ? (index == 0 ? size - 1 : index - 1) : (index + 1 == size ? 0 : index + 1);
        Memory::Write8(player + kCycleIndex, uint8_t(index));
        SetSlot(ctx, saved, player, kCycleIds[index]);
    } else if (mode == LocalVs::ItemCycler::Infinite) {
        if (index != 0 && Memory::Read32(player + kSlotItem) != kCycleIds[index]) {
            SetSlot(ctx, saved, player, kCycleIds[index]);
        }
    } else if ((Memory::Read16(player + kUseFlags) & 0x46) == 0x40) {
        SetSlot(ctx, saved, player, kCycleIds[index]);  // debug: use with an empty slot refills it
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x80797928, ItemCycler_PlayerUpdate_80797928, "ItemCycler_PlayerUpdate_80797928");
