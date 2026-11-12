// mkw100: caps the audio sources one sort pass sees, so more than 12 players fit its stack buckets.
//
// 0x80712B58 (called from Audio::RaceMgr::Calc) walks the source list at this+0x78 and copies each
// source into a {source, distance} stack array by its type (+0xC), then sorts each array by
// distance. The arrays are sized for 12 players: type 7 holds 10, type 4 and types 5/6 hold 12,
// type 3 holds 60, types 0-2 hold 300. A 50-player balloon battle overfilled one and wrote over the
// saved registers of the callers (crash returning into Audio::RaceMgr::Calc). Sources past a
// bucket's capacity are unlinked for the call, in list order, and linked back afterwards; with 12
// or fewer players nothing is ever unlinked. Nearest-first selection over the dropped ones would
// need the distance pass natively; list order is the simplification.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <vector>

namespace {

// nw4r::ut::List: {head, tail, u16 count, u16 link offset}; ut::Link at object+offset: {prev, next}.
constexpr uint32_t kSourceList = 0x78;
constexpr uint32_t kSourceType = 0xC;

uint32_t BucketCapacity(uint32_t type) {
    switch (type) {
    case 7: return 10;
    case 4: return 12;
    case 5: case 6: return 12;
    case 3: return 60;
    default: return type <= 2 ? 300 : UINT32_MAX;
    }
}

uint32_t BucketOf(uint32_t type) { return type == 6 ? 5 : (type <= 2 ? 0 : type); }

struct Unlinked {
    uint32_t obj;
    uint32_t prev;  // object it followed (0 = was the head)
};

} // namespace

extern "C" void AudioSourceSort_80712b58(CpuContext* ctx) {
    const uint32_t list = ctx->gpr[3] + kSourceList;
    const uint32_t off = Memory::Read16(list + 0xA);
    uint32_t counts[8] = {};
    std::vector<Unlinked> removed;
    uint32_t prevKept = 0;
    for (uint32_t obj = Memory::Read32(list); obj != 0;) {
        const uint32_t next = Memory::Read32(obj + off + 4);
        const uint32_t type = Memory::Read32(obj + kSourceType);
        const uint32_t b = BucketOf(type);
        if (b < 8 && ++counts[b] > BucketCapacity(type)) {
            const uint32_t prev = Memory::Read32(obj + off);
            if (prev != 0) Memory::Write32(prev + off + 4, next); else Memory::Write32(list, next);
            if (next != 0) Memory::Write32(next + off, prev); else Memory::Write32(list + 4, prev);
            Memory::Write16(list + 8, Memory::Read16(list + 8) - 1);
            removed.push_back({obj, prevKept});
        } else {
            prevKept = obj;
        }
        obj = next;
    }
    MKW_GUEST_NEXT(80712B58)(ctx);
    // Back in reverse so each prev is in the list again (removed ones never precede each other's
    // anchors: an anchor is always a kept object or the head).
    for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
        const uint32_t obj = it->obj;
        const uint32_t next = it->prev != 0 ? Memory::Read32(it->prev + off + 4) : Memory::Read32(list);
        Memory::Write32(obj + off, it->prev);
        Memory::Write32(obj + off + 4, next);
        if (it->prev != 0) Memory::Write32(it->prev + off + 4, obj); else Memory::Write32(list, obj);
        if (next != 0) Memory::Write32(next + off, obj); else Memory::Write32(list + 4, obj);
        Memory::Write16(list + 8, Memory::Read16(list + 8) + 1);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80712B58, AudioSourceSort_80712b58, "AudioSourceSort_80712b58");
