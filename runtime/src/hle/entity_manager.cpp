// mkw100: EntityManager (Kinoko's BoxColManager) past 256 entities.
//
// The game's spatial index for everything with dynamic collision (karts, items on the track,
// course objects) holds 256 units: its sweep-and-prune points store unit and point numbers in
// bytes, and InsertEntity returns null once 256 are in. A unit that did not fit has no collision
// at all, so with many racers (more so with Item Fever's item rain) karts drove through each other
// and through items on the ground. Every function that touches the points is run natively here
// with int indices and kMaxUnits units; the algorithm is the game's (ported from Kinoko,
// https://github.com/vabold/Kinoko, MIT, source/game/field/BoxColManager.cc), so the order of
// search results, and with it every race up to 256 units, is unchanged.
//
// The guest object keeps the fields other code reads: the unit pool and search results move to
// kPool / kResults (pointers +0x18 / +0x1C); the count, maxID, the search cache and the four
// iterators (+0x428..+0x434) stay where they were and keep their meaning, because Pulsar packs
// read and write them too (Retro Rewind replaces UpdateIndexes, 0x80785F2C, with its own that
// reorders the results and sets the iterators). An iterator is a result index, 256 at its end as
// the game's callers test; past 256 results, result 256 itself is skipped so it cannot read as
// the end.
//
// Guest layout (EntityManager, from its constructor 0x807856E0):
//   +0x10 high points*  +0x14 low points*  +0x18 unit pool*  +0x1C search results*
//   +0x20 unit id free list [256]  +0x420 count  +0x424 next unit id
//   +0x428 kart / +0x42C item / +0x430 object / +0x434 drivable iterator  +0x438 maxID
//   +0x43C cache unit  +0x440 cache point  +0x44C cache radius  +0x450 cache flag
// Entity (unit, 0x20): +0 pos*  +4 radius  +8 range  +0xC flag  +0x10 user data
//   +0x14 s16 high point  +0x16 s16 low point  +0x18 xMax  +0x1C xMin
// Flags: 1 kart, 2 item, 4/8 object, 0x10 drivable object, 0x100 recalc every frame,
// 0x200 intangible, 0x400 active, 0x800 recalc once.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kMaxUnits = 2048;
constexpr uint32_t kGameMaxUnits = 256;
constexpr uint32_t kUnitSize = 0x20;
// Array shadow class span 12 (array_overflow.h), unused by any array class.
constexpr uint32_t kPool = 0x82C00000;                       // kMaxUnits units
constexpr uint32_t kResults = kPool + kMaxUnits * kUnitSize;  // kMaxUnits unit pointers

constexpr uint32_t kHighPoints = 0x10, kLowPoints = 0x14, kUnitPool = 0x18, kUnitList = 0x1C;
constexpr uint32_t kCount = 0x420, kNextUnit = 0x424, kIterators = 0x428, kMaxId = 0x438;
constexpr uint32_t kCacheUnit = 0x43C, kCachePoint = 0x440, kCacheRadius = 0x44C, kCacheFlag = 0x450;

constexpr uint32_t kUPos = 0x0, kURadius = 0x4, kURange = 0x8, kUFlag = 0xC, kUData = 0x10;
constexpr uint32_t kUHigh = 0x14, kULow = 0x16, kUXMax = 0x18, kUXMin = 0x1C;

constexpr uint32_t kFlagIntangible = 0x200, kFlagActive = 0x400;
constexpr uint32_t kFlagRecalc = 0x100, kFlagRecalcOnce = 0x800;
constexpr uint32_t kIteratorFlags[4] = {0x1, 0x2, 0xC, 0x10};  // kart, item, object, drivable

struct HighPoint { float z; int lowPoint; int minLowPoint; };
struct LowPoint { float z; int highPoint; int unitId; };

struct Manager {
    uint32_t guest = 0;
    uint32_t origPool = 0, origList = 0;  // the game's own allocations, given back before its dtor
    std::vector<HighPoint> high;
    std::vector<LowPoint> low;
    std::vector<int> unitIds;
    int count = 0, nextUnitId = 0, maxId = 0;
    bool overLogged = false;
};
Manager g_mgr;

uint32_t Unit(int id) { return kPool + uint32_t(id) * kUnitSize; }
int UnitId(uint32_t unit) { return int((unit - kPool) / kUnitSize); }
float RF(uint32_t a) { return Memory::ReadFloat32(a); }
void WF(uint32_t a, float v) { Memory::WriteFloat32(a, v); }
uint32_t R32(uint32_t a) { return Memory::Read32(a); }
void W32(uint32_t a, uint32_t v) { Memory::Write32(a, v); }
int RS16(uint32_t a) { return int(int16_t(Memory::Read16(a))); }
void WS16(uint32_t a, int v) { Memory::Write16(a, uint16_t(int16_t(v))); }
void AddS16(uint32_t a, int d) { WS16(a, RS16(a) + d); }

void SetCount(int count) {
    g_mgr.count = count;
    W32(g_mgr.guest + kCount, uint32_t(count));
}

void SetMaxId(int maxId) {
    g_mgr.maxId = maxId;
    W32(g_mgr.guest + kMaxId, uint32_t(maxId));
}

// Called on the first insert after the game's constructor (it inserts two bound units itself).
void Adopt(uint32_t mgr) {
    g_mgr = Manager{};
    g_mgr.guest = mgr;
    g_mgr.origPool = R32(mgr + kUnitPool);
    g_mgr.origList = R32(mgr + kUnitList);
    g_mgr.high.assign(kMaxUnits, HighPoint{});
    g_mgr.low.assign(kMaxUnits, LowPoint{});
    g_mgr.unitIds.resize(kMaxUnits);
    std::iota(g_mgr.unitIds.begin(), g_mgr.unitIds.end(), 1);
    for (uint32_t i = 0; i < kMaxUnits * kUnitSize; i += 4) Memory::Write32(kPool + i, 0);
    for (uint32_t i = 0; i < kMaxUnits; ++i) Memory::Write32(kResults + 4 * i, 0);
    W32(mgr + kUnitPool, kPool);
    W32(mgr + kUnitList, kResults);
    SetCount(int(R32(mgr + kCount)));  // 0: the constructor inserts before anything else
    g_mgr.nextUnitId = int(R32(mgr + kNextUnit));
}

bool Ours(uint32_t mgr) { return mgr != 0 && mgr == g_mgr.guest && R32(mgr + kUnitPool) == kPool; }

uint32_t IteratorField(int which) { return g_mgr.guest + kIterators + 4 * uint32_t(which); }

// The next result after `from` with the iterator's flags, or 256.
void Iterate(int which, int from) {
    for (int it = from + 1; it < g_mgr.maxId; ++it) {
        if (it == int(kGameMaxUnits)) continue;  // 256 is the end marker
        if ((R32(R32(kResults + 4 * uint32_t(it)) + kUFlag) & kIteratorFlags[which]) != 0) {
            W32(IteratorField(which), uint32_t(it));
            return;
        }
    }
    W32(IteratorField(which), kGameMaxUnits);
}

void ResetIterators() {
    for (int i = 0; i < 4; ++i) Iterate(i, -1);
}

uint32_t GetNext(int which) {
    const int it = int(R32(IteratorField(which)));
    if (it == int(kGameMaxUnits) || it < 0 || it >= g_mgr.maxId) return 0;
    const uint32_t unit = R32(kResults + 4 * uint32_t(it));
    Iterate(which, it);
    return R32(unit + kUData);
}

void Clear() {
    for (int i = 0; i < 4; ++i) W32(IteratorField(i), kGameMaxUnits);
    SetMaxId(0);
    W32(g_mgr.guest + kCacheUnit, 0);
    WF(g_mgr.guest + kCacheRadius, -1.0f);
    W32(g_mgr.guest + kCacheFlag, 0);
}

void InitUnit(uint32_t unit, float radius, float maxSpeed, uint32_t pos, uint32_t flag, uint32_t userData) {
    const float range = radius + maxSpeed;
    W32(unit + kUPos, pos);
    WF(unit + kURadius, radius);
    WF(unit + kURange, range);
    W32(unit + kUFlag, flag | kFlagActive);
    W32(unit + kUData, userData);
    const float x = RF(pos);
    WF(unit + kUXMax, x + range);
    WF(unit + kUXMin, x - range);
}

uint32_t Insert(float radius, float maxSpeed, uint32_t pos, uint32_t flag, uint32_t userData) {
    Manager& m = g_mgr;
    if (m.count >= int(kMaxUnits)) return 0;
    if (m.count >= int(kGameMaxUnits) && !m.overLogged) {
        m.overLogged = true;
        RT_LOGF("entities", "more than %u entities with collision (the game's limit); all of them collide\n", kGameMaxUnits);
    }
    const int unitId = m.nextUnitId;
    const uint32_t unit = Unit(unitId);
    InitUnit(unit, radius, maxSpeed, pos, flag, userData);
    m.nextUnitId = m.unitIds[unitId];
    W32(m.guest + kNextUnit, uint32_t(m.nextUnitId));
    const float range = radius + maxSpeed;
    const float z = RF(pos + 8);
    const float zHigh = z + range, zLow = z - range;

    if (m.count == 0) {
        m.high[0] = {zHigh, 0, 0};
        m.low[0] = {zLow, 0, unitId};
        WS16(Unit(0) + kUHigh, 0);  // as the game: unit 0, which is this one when the index is empty
        WS16(Unit(0) + kULow, 0);
        SetCount(1);
        return unit;
    }

    int highIdx = 0, lowIdx = 0;
    for (int i = m.count;;) {
        const int hs = highIdx + i, ls = lowIdx + i;
        if (hs <= m.count && zHigh > m.high[hs - 1].z) highIdx = hs;
        if (ls <= m.count && zLow > m.low[ls - 1].z) lowIdx = ls;
        if (i == 1) break;
        i = (i + 1) / 2;
    }
    WS16(unit + kUHigh, highIdx);
    WS16(unit + kULow, lowIdx);

    for (int i = m.count; i > highIdx; --i) {
        m.high[i] = m.high[i - 1];
        HighPoint& h = m.high[i];
        LowPoint& l = m.low[h.lowPoint];
        ++l.highPoint;
        AddS16(Unit(l.unitId) + kUHigh, 1);
        if (h.minLowPoint >= lowIdx) ++h.minLowPoint;
    }
    m.high[highIdx].lowPoint = lowIdx;
    m.high[highIdx].z = zHigh;
    if (highIdx == m.count || m.high[highIdx + 1].minLowPoint > lowIdx) {
        m.high[highIdx].minLowPoint = lowIdx;
        for (int i = highIdx - 1; i >= 0 && m.high[i].minLowPoint > lowIdx; --i) m.high[i].minLowPoint = lowIdx;
    } else {
        m.high[highIdx].minLowPoint = m.high[highIdx + 1].minLowPoint;
    }

    for (int i = m.count; i > lowIdx; --i) {
        m.low[i] = m.low[i - 1];
        LowPoint& l = m.low[i];
        ++m.high[l.highPoint].lowPoint;
        AddS16(Unit(l.unitId) + kULow, 1);
    }
    m.low[lowIdx] = {zLow, highIdx, unitId};
    SetCount(m.count + 1);
    return unit;
}

void Remove(uint32_t unit) {
    Manager& m = g_mgr;
    if (unit == 0 || (R32(unit + kUFlag) & kFlagActive) == 0) return;
    const int highIdx = RS16(unit + kUHigh), lowIdx = RS16(unit + kULow);

    for (int i = highIdx; i < m.count - 1; ++i) {
        m.high[i] = m.high[i + 1];
        HighPoint& h = m.high[i];
        LowPoint& l = m.low[h.lowPoint];
        --l.highPoint;
        AddS16(Unit(l.unitId) + kUHigh, -1);
        if (h.minLowPoint > lowIdx) --h.minLowPoint;
    }
    for (int i = lowIdx; i < m.count - 1; ++i) {
        m.low[i] = m.low[i + 1];
        LowPoint& l = m.low[i];
        --m.high[l.highPoint].lowPoint;
        AddS16(Unit(l.unitId) + kULow, -1);
        if (l.highPoint >= highIdx) continue;
        int minLow = m.high[l.highPoint].minLowPoint;
        if (minLow != lowIdx) continue;
        while (m.low[minLow].highPoint < l.highPoint) ++minLow;
        m.high[l.highPoint].minLowPoint = minLow;
    }

    W32(unit + kUFlag, R32(unit + kUFlag) & ~kFlagActive);
    const int id = UnitId(unit);
    m.unitIds[id] = m.nextUnitId;
    m.nextUnitId = id;
    W32(m.guest + kNextUnit, uint32_t(id));
    SetCount(m.count - 1);
}

void Calc() {
    Manager& m = g_mgr;
    Clear();
    int active = 0;
    for (uint32_t id = 0; id < kMaxUnits && m.count > 0; ++id) {
        const uint32_t unit = Unit(int(id));
        const uint32_t flag = R32(unit + kUFlag);
        if ((flag & kFlagActive) == 0) continue;
        if ((flag & (kFlagRecalc | kFlagRecalcOnce)) != 0) {
            const uint32_t pos = R32(unit + kUPos);
            const float range = RF(unit + kURange);
            const float x = RF(pos), z = RF(pos + 8);
            WF(unit + kUXMax, x + range);
            WF(unit + kUXMin, x - range);
            m.high[RS16(unit + kUHigh)].z = z + range;
            m.low[RS16(unit + kULow)].z = z - range;
            W32(unit + kUFlag, flag & ~kFlagRecalcOnce);
        }
        if (++active >= m.count) break;
    }

    for (int i = 1; i < m.count; ++i) {
        for (int j = i; j >= 1 && m.high[j - 1].z > m.high[j].z; --j) {
            std::swap(m.high[j], m.high[j - 1]);
            HighPoint& upper = m.high[j];
            HighPoint& lower = m.high[j - 1];
            LowPoint& upperLow = m.low[upper.lowPoint];
            LowPoint& lowerLow = m.low[lower.lowPoint];
            ++upperLow.highPoint;
            --lowerLow.highPoint;
            AddS16(Unit(upperLow.unitId) + kUHigh, 1);
            AddS16(Unit(lowerLow.unitId) + kUHigh, -1);
            int& nextMin = upper.minLowPoint;
            if (nextMin == lower.lowPoint) {
                do { ++nextMin; } while (m.low[nextMin].highPoint < j);
            }
            lower.minLowPoint = std::min(lower.minLowPoint, upper.lowPoint);
        }
    }

    for (int i = 1; i < m.count; ++i) {
        for (int j = i; j >= 1 && m.low[j - 1].z > m.low[j].z; --j) {
            std::swap(m.low[j], m.low[j - 1]);
            LowPoint& upper = m.low[j];
            LowPoint& lower = m.low[j - 1];
            ++m.high[upper.highPoint].lowPoint;
            --m.high[lower.highPoint].lowPoint;
            AddS16(Unit(upper.unitId) + kULow, 1);
            AddS16(Unit(lower.unitId) + kULow, -1);
            if (upper.highPoint > lower.highPoint) {
                int k = upper.highPoint;
                while (k > lower.highPoint && m.high[k].minLowPoint == j - 1) ++m.high[k--].minLowPoint;
            } else {
                int k = lower.highPoint;
                while (k > upper.highPoint && m.high[k].minLowPoint == j) --m.high[k--].minLowPoint;
            }
        }
    }
}

void AddResult(uint32_t unit) {
    W32(kResults + 4 * uint32_t(g_mgr.maxId), unit);
    SetMaxId(g_mgr.maxId + 1);
}

void SearchUnit(uint32_t unit, uint32_t flag) {
    Manager& m = g_mgr;
    if ((R32(unit + kUFlag) & kFlagActive) == 0) return;
    int highIdx = RS16(unit + kUHigh), lowIdx = RS16(unit + kULow);
    const int origLow = lowIdx;
    const float highZ = m.high[highIdx].z, lowZ = m.low[origLow].z;
    const float xMax = RF(unit + kUXMax), xMin = RF(unit + kUXMin);
    const uint32_t pos = R32(unit + kUPos);
    const float radius = RF(unit + kURadius);
    const float px = RF(pos), pz = RF(pos + 8);
    const float zHigh = pz + radius, zLow = pz - radius, xHigh = px + radius, xLow = px - radius;
    const int maxIdx = m.count - 1;

    SetMaxId(0);
    W32(m.guest + kCacheUnit, unit);
    WF(m.guest + kCacheRadius, -1.0f);
    W32(m.guest + kCacheFlag, flag);

    while (highIdx > 0 && m.high[highIdx - 1].z >= lowZ) --highIdx;
    while (lowIdx < maxIdx && m.low[lowIdx + 1].z <= highZ) ++lowIdx;
    const int minLow = m.high[highIdx].minLowPoint;
    for (; lowIdx >= minLow; --lowIdx) {
        const LowPoint& l = m.low[lowIdx];
        if (l.highPoint >= highIdx && lowIdx != origLow) {
            const uint32_t other = Unit(l.unitId);
            if (RF(other + kUXMax) < xMin || RF(other + kUXMin) > xMax) continue;
            const uint32_t oflag = R32(other + kUFlag);
            if ((oflag & flag) == 0 || (oflag & kFlagIntangible) != 0) continue;
            const float r = RF(other + kURadius);
            const uint32_t opos = R32(other + kUPos);
            const float ox = RF(opos), oz = RF(opos + 8);
            if (oz + r < zLow || oz - r > zHigh) continue;
            if (ox + r < xLow || ox - r > xHigh) continue;
            AddResult(other);
            if (m.maxId == int(kMaxUnits)) break;
        }
        if (lowIdx == 0) break;
    }
}

void SearchSphere(float radius, uint32_t posAddr, uint32_t flag) {
    Manager& m = g_mgr;
    const float px = RF(posAddr), py = RF(posAddr + 4), pz = RF(posAddr + 8);
    const float zHigh = pz + radius, zLow = pz - radius, xHigh = px + radius, xLow = px - radius;
    SetMaxId(0);
    W32(m.guest + kCacheUnit, 0);
    WF(m.guest + kCachePoint, px);
    WF(m.guest + kCachePoint + 4, py);
    WF(m.guest + kCachePoint + 8, pz);
    WF(m.guest + kCacheRadius, radius);
    W32(m.guest + kCacheFlag, flag);

    int highIdx = 0, lowIdx = 0;
    for (int i = m.count - 1;;) {
        const int hs = highIdx + i, ls = lowIdx + i;
        if (hs <= m.count && zLow > m.high[hs - 1].z) highIdx = hs;
        if (ls <= m.count && zHigh >= m.low[ls].z) lowIdx = ls;
        if (i == 1) break;
        i = (i + 1) / 2;
    }
    const int minLow = m.high[highIdx].minLowPoint;
    for (; lowIdx >= minLow; --lowIdx) {
        const LowPoint& l = m.low[lowIdx];
        if (l.highPoint >= highIdx) {
            const uint32_t unit = Unit(l.unitId);
            if (RF(unit + kUXMax) < xLow || RF(unit + kUXMin) > xHigh) continue;
            const uint32_t uflag = R32(unit + kUFlag);
            if ((uflag & flag) == 0 || (uflag & kFlagIntangible) != 0) continue;
            AddResult(unit);
            if (m.maxId == int(kMaxUnits)) break;
        }
        if (lowIdx == 0) break;
    }
}

} // namespace

// EntityManager::__ct: the game allocates and inserts its two bound units; the first insert
// adopts the new instance (Insert below).
extern "C" void EntityManager_Ct_807856e0(CpuContext* ctx) {
    g_mgr.guest = 0;
    MKW_GUEST_NEXT(807856E0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807856E0, EntityManager_Ct_807856e0, "EntityManager_Ct_807856e0");

// EntityManager::__dt: give the game back its own arrays to free.
extern "C" void EntityManager_Dt_807854e4(CpuContext* ctx) {
    const uint32_t mgr = ctx->gpr[3];
    if (Ours(mgr)) {
        W32(mgr + kUnitPool, g_mgr.origPool);
        W32(mgr + kUnitList, g_mgr.origList);
        g_mgr.guest = 0;
    }
    MKW_GUEST_NEXT(807854E4)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807854E4, EntityManager_Dt_807854e4, "EntityManager_Dt_807854e4");

// InsertEntity(this, f1 radius, f2 maxSpeed, r4 pos, r5 flag, r6 userData) -> unit or null
extern "C" void EntityManager_InsertEntity_80786134(CpuContext* ctx) {
    const uint32_t mgr = ctx->gpr[3];
    // MKW100_TEST_GAME_ENTITIES (tests only): keep the game's own 256-unit manager.
    static const bool gameEntities = std::getenv("MKW100_TEST_GAME_ENTITIES") != nullptr;
    if (gameEntities) { MKW_GUEST_NEXT(80786134)(ctx); return; }
    if (!Ours(mgr)) Adopt(mgr);
    ctx->gpr[3] = Insert(float(ctx->fpr[1].d), float(ctx->fpr[2].d), ctx->gpr[4], ctx->gpr[5], ctx->gpr[6]);
}
REGISTER_NATIVE_WRAPPER_AS(0x80786134, EntityManager_InsertEntity_80786134, "EntityManager_InsertEntity_80786134");

// RemoveEntity(this, Entity** unit)
extern "C" void EntityManager_RemoveEntity_80786578(CpuContext* ctx) {
    const uint32_t mgr = ctx->gpr[3], slot = ctx->gpr[4];
    if (!Ours(mgr)) { MKW_GUEST_NEXT(80786578)(ctx); return; }
    const uint32_t unit = R32(slot);
    if (unit == 0 || (R32(unit + kUFlag) & kFlagActive) == 0) return;
    Remove(unit);
    W32(slot, 0);
}
REGISTER_NATIVE_WRAPPER_AS(0x80786578, EntityManager_RemoveEntity_80786578, "EntityManager_RemoveEntity_80786578");

extern "C" void EntityManager_Clear_8078597c(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(8078597C)(ctx); return; }
    Clear();
}
REGISTER_NATIVE_WRAPPER_AS(0x8078597C, EntityManager_Clear_8078597c, "EntityManager_Clear_8078597c");

extern "C" void EntityManager_Update_807859b0(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(807859B0)(ctx); return; }
    Calc();
}
REGISTER_NATIVE_WRAPPER_AS(0x807859B0, EntityManager_Update_807859b0, "EntityManager_Update_807859b0");

// The iterators (written out: the build finds wrappers by their REGISTER_NATIVE_WRAPPER_AS text).
// GetNextKartPlayer
extern "C" void EntityManager_GetNext_80785d8c(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80785D8C)(ctx); return; }
    ctx->gpr[3] = GetNext(0);
}
REGISTER_NATIVE_WRAPPER_AS(0x80785D8C, EntityManager_GetNext_80785d8c, "EntityManager_GetNext_80785d8c");

// GetNextObj (items)
extern "C" void EntityManager_GetNext_80785df4(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80785DF4)(ctx); return; }
    ctx->gpr[3] = GetNext(1);
}
REGISTER_NATIVE_WRAPPER_AS(0x80785DF4, EntityManager_GetNext_80785df4, "EntityManager_GetNext_80785df4");

// GetNextObjectCollidable
extern "C" void EntityManager_GetNext_80785e5c(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80785E5C)(ctx); return; }
    ctx->gpr[3] = GetNext(2);
}
REGISTER_NATIVE_WRAPPER_AS(0x80785E5C, EntityManager_GetNext_80785e5c, "EntityManager_GetNext_80785e5c");

// GetNextObjectKCL
extern "C" void EntityManager_GetNext_80785ec4(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80785EC4)(ctx); return; }
    ctx->gpr[3] = GetNext(3);
}
REGISTER_NATIVE_WRAPPER_AS(0x80785EC4, EntityManager_GetNext_80785ec4, "EntityManager_GetNext_80785ec4");

// FindEntitiesCloseToEntity(this, r4 unit, r5 flag)
extern "C" void EntityManager_FindCloseToEntity_807868c0(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(807868C0)(ctx); return; }
    SearchUnit(ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_WRAPPER_AS(0x807868C0, EntityManager_FindCloseToEntity_807868c0, "EntityManager_FindCloseToEntity_807868c0");

// FindEntitiesCloseToSphere(this, f1 radius, r4 pos, r5 flag)
extern "C" void EntityManager_FindCloseToSphere_80786c60(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80786C60)(ctx); return; }
    SearchSphere(float(ctx->fpr[1].d), ctx->gpr[4], ctx->gpr[5]);
}
REGISTER_NATIVE_WRAPPER_AS(0x80786C60, EntityManager_FindCloseToSphere_80786c60, "EntityManager_FindCloseToSphere_80786c60");

// PrepareFromEntity / PrepareFromSphere: the search plus the iterator reset (inlined in the game).
extern "C" void EntityManager_PrepareFromEntity_80786774(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80786774)(ctx); return; }
    SearchUnit(ctx->gpr[4], ctx->gpr[5]);
    ResetIterators();
}
REGISTER_NATIVE_WRAPPER_AS(0x80786774, EntityManager_PrepareFromEntity_80786774, "EntityManager_PrepareFromEntity_80786774");

extern "C" void EntityManager_PrepareFromSphere_80786b14(CpuContext* ctx) {
    if (!Ours(ctx->gpr[3])) { MKW_GUEST_NEXT(80786B14)(ctx); return; }
    SearchSphere(float(ctx->fpr[1].d), ctx->gpr[4], ctx->gpr[5]);
    ResetIterators();
}
REGISTER_NATIVE_WRAPPER_AS(0x80786B14, EntityManager_PrepareFromSphere_80786b14, "EntityManager_PrepareFromSphere_80786b14");
