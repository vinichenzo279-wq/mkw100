// mkw100: Retro Rewind with Item Fever's items (experimental, docs/mkw100/rr-item-fever.md).
//
// The combined Kamek module (scripts/mkw100/combine_pul.py) carries both packs' code; this file
// holds the run-time side of that build. For now: a test hook that hands CPUs a chosen item.

#include "hle/rr_item_fever.h"

#include "abi_bridge.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint32_t kItemManager = 0x809C3618;   // Item::Manager::sInstance (PAL)
constexpr uint32_t kItemPlayerSize = 0x248;      // Item::Player
constexpr uint32_t kInventory = 0x88;            // Item::Player::inventory
constexpr uint32_t kSetItem = 0x807BC940;        // Item::PlayerInventory::SetItem(id, forced)
constexpr uint32_t kItemNone = 0x14;

long GiveItemFromEnv() {
    const char* text = std::getenv("MKW100_TEST_GIVE_ITEM");
    if (!text || !*text) return -1;
    return std::strtol(text, nullptr, 0);
}

} // namespace

namespace RrItemFever {

// MKW100_TEST_GIVE_ITEM=<item id>: from race frame 600, every 300 frames, every CPU holding nothing
// gets that item through the game's own (patched) Item::PlayerInventory::SetItem, so the item's
// whole path runs: inventory, CPU decision to use it, its object, hits. Logs how many of the
// previous round's items were used.
void TestGiveItem(CpuContext* ctx, uint32_t raceinfo) {
    static const long item = GiveItemFromEnv();
    if (item < 0) return;
    if (Memory::Read32(raceinfo + 0x28) != 2) return;  // racing
    const uint32_t frame = Memory::Read32(raceinfo + 0x20);
    if (frame < 600 || frame % 300 != 0) return;
    const uint32_t manager = Memory::Read32(kItemManager);
    if (!manager) return;
    const uint32_t count = Memory::Read8(manager + 0x10);
    const uint32_t players = Memory::Read32(manager + 0x14);
    static uint32_t lastGiven = 0;
    uint32_t stillHeld = 0, given = 0, other = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t player = players + i * kItemPlayerSize;
        if (Memory::Read8(player + 0x19)) continue;  // human
        const uint32_t inventory = player + kInventory;
        const uint32_t held = Memory::Read32(inventory + 4);
        if (held == uint32_t(item)) { ++stillHeld; continue; }
        if (held != kItemNone) { ++other; continue; }
        const CpuContext saved = *ctx;
        ctx->gpr[3] = inventory;
        ctx->gpr[4] = uint32_t(item);
        ctx->gpr[5] = 0;
        InvokeIndirectCpu(kSetItem, ctx);
        *ctx = saved;
        given += Memory::Read32(inventory + 4) == uint32_t(item);
    }
    RT_LOGF("rr-item-fever", "frame %u: gave item 0x%lX to %u CPUs; of last round's %u, %u still held, "
            "%u CPUs hold something else\n", frame, item, given, lastGiven, stillHeld, other);
    lastGiven = given + stillHeld;
}

} // namespace RrItemFever
