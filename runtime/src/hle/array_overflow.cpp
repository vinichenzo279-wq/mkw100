// mkw100 shadow windows for per-player arrays in singletons: the helpers behind the translator's
// rewritten scale and step sites, and the creation hooks that set the windows up.
// Layout and rationale: runtime/include/guest_abi/array_overflow.h.

#include "guest_abi/array_overflow.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <iterator>
#include <stdexcept>
#include <string>

namespace ArrayOverflow {

const ArrayClass& Class(uint32_t id) {
    if (id == 0 || id > std::size(kClasses)) {
        throw std::runtime_error("mkw100 array overflow: unknown class id " + std::to_string(id));
    }
    return kClasses[id - 1];
}

uint32_t Instance(const ArrayClass& c) {
    if (c.member == kStaticObject) {
        return c.instanceGlobal;
    }
    const uint32_t singleton = Memory::Read32(c.instanceGlobal);
    if (c.member < 0 || singleton == 0) {
        return singleton;
    }
    return Memory::Read32(singleton + static_cast<uint32_t>(c.member));
}

void InitWindows(const ArrayClass& c, CpuContext* ctx) {
    const uint32_t begin = ClassBase(c.id);
    const uint32_t end = begin + kWindows * c.window();
    for (uint32_t address = begin; address < end; address += 4) {
        Memory::Write32(address, 0);
    }
    if (c.elementCtor == 0) {
        return;
    }
    for (const uint32_t array : c.ctorArrays) {
        if (array == kNoArray) {
            continue;
        }
        for (uint32_t index = kInlineSlots; index < kMaxPlayers; ++index) {
            ctx->gpr[3] = Window(c, index) + array;
            InvokeIndirectCpu(c.elementCtor, ctx);
        }
    }
}

uint32_t ElementIndex(const ArrayClass& c, uint32_t pointer, uint32_t origin, uint32_t stride) {
    const uint32_t instance = Instance(c);
    if (instance == 0) {
        return kNoArray;
    }
    const uint32_t inlineOffset = pointer - instance - origin;
    if (inlineOffset < kInlineSlots * stride && inlineOffset % stride == 0) {
        return inlineOffset / stride;
    }
    // Element i >= 12 of the array at `origin` sits at Window(c, i) + origin (MKW100_IndexScaled).
    const uint32_t windowOffset = pointer - (Window(c, kInlineSlots) + origin);
    if (windowOffset < kWindows * c.window() && windowOffset % c.window() == 0) {
        return kInlineSlots + windowOffset / c.window();
    }
    return kNoArray;
}

} // namespace ArrayOverflow

using namespace ArrayOverflow;

namespace {

// Wraps a createInstance: when it creates the singleton, set up every class rooted at its global.
void CreateInstanceHook(CpuContext* ctx, uint32_t global, void (*next)(CpuContext*)) {
    const uint32_t before = Memory::Read32(global);
    next(ctx);
    if (before != 0 || Memory::Read32(global) == 0) {
        return;
    }
    CpuContext saved = *ctx;
    for (const ArrayClass& c : kClasses) {
        if (c.instanceGlobal == global) {
            InitWindows(c, ctx);
        }
    }
    *ctx = saved;
}

// Wraps the constructor of a class whose sites include it (array-classes.tsv ctor column): the
// game stores the instance in the global only after the constructor returns, so publish it first
// for the helpers to recognize, and set up the windows before the constructor writes them.
void ConstructorHook(CpuContext* ctx, uint32_t classId, void (*next)(CpuContext*)) {
    const ArrayClass& c = Class(classId);
    Memory::Write32(c.instanceGlobal, ctx->gpr[3]);
    CpuContext saved = *ctx;
    InitWindows(c, ctx);
    *ctx = saved;
    next(ctx);
}

} // namespace

extern "C" uint32_t MKW100_IndexScaled(uint32_t scaled, uint32_t classId, uint32_t stride) {
    const uint32_t index = scaled / stride;
    // Out-of-range indices keep the original arithmetic, whatever they meant.
    if (index < kInlineSlots || index >= kMaxPlayers) {
        return scaled;
    }
    const ArrayClass& c = Class(classId);
    const uint32_t instance = Instance(c);
    if (instance == 0) {
        return scaled;
    }
    return Window(c, index) - instance;
}

extern "C" uint32_t MKW100_IndexedBase(uint32_t base, uint32_t offset, uint32_t table, uint32_t size,
                                       uint32_t shadow) {
    if (base != table || offset < kInlineSlots * size || offset >= kMaxPlayers * size) {
        return base;
    }
    return shadow - kInlineSlots * size;
}

extern "C" uint32_t MKW100_IndexedRestore(uint32_t base, uint32_t table, uint32_t size, uint32_t shadow) {
    return base == shadow - kInlineSlots * size ? table : base;
}

extern "C" uint32_t MKW100_ArrayStep(uint32_t pointer, uint32_t classId, uint32_t stride, uint32_t origin) {
    const ArrayClass& c = Class(classId);
    const uint32_t instance = Instance(c);
    if (instance != 0 && pointer == instance + origin + (kInlineSlots - 1) * stride) {
        return Window(c, kInlineSlots) + origin;
    }
    // Inside window k (pointer == Window(12 + k) + origin): move to window k + 1. One past the
    // last window stays inside the class span; the loop bound stops there.
    const uint32_t relative = pointer - origin - Window(c, kInlineSlots);
    if (relative % c.window() == 0 && relative / c.window() < kWindows) {
        return pointer + c.window();
    }
    return pointer + stride;
}

// AI::Manager::createInstance allocates 0x9C bytes and constructs, allocating the member objects.
extern "C" void AIManager_CreateInstance_80738e50(CpuContext* ctx) {
    CreateInstanceHook(ctx, Class(1).instanceGlobal, MKW_GUEST_NEXT(80738E50));
}
REGISTER_NATIVE_WRAPPER_AS(0x80738E50, AIManager_CreateInstance_80738e50, "AIManager_CreateInstance_80738e50");

// Input::Manager::CreateInstance allocates 0x415C bytes and constructs.
extern "C" void InputManager_CreateInstance_8052313c(CpuContext* ctx) {
    CreateInstanceHook(ctx, Class(5).instanceGlobal, MKW_GUEST_NEXT(8052313C));
}
REGISTER_NATIVE_WRAPPER_AS(0x8052313C, InputManager_CreateInstance_8052313c, "InputManager_CreateInstance_8052313c");

// Item::Manager::__ct; Item::Manager::CreateInstance (0x80799138) stores the same pointer after it.
extern "C" void ItemManager_Ctor_807992d8(CpuContext* ctx) {
    ConstructorHook(ctx, 6, MKW_GUEST_NEXT(807992D8));
}
REGISTER_NATIVE_WRAPPER_AS(0x807992D8, ItemManager_Ctor_807992d8, "ItemManager_Ctor_807992d8");

// Item::GessoMgr::CreateInstance allocates 0x54 bytes and constructs.
extern "C" void GessoMgr_CreateInstance_807a8f04(CpuContext* ctx) {
    CreateInstanceHook(ctx, Class(7).instanceGlobal, MKW_GUEST_NEXT(807A8F04));
}
REGISTER_NATIVE_WRAPPER_AS(0x807A8F04, GessoMgr_CreateInstance_807a8f04, "GessoMgr_CreateInstance_807a8f04");

// ArchiveMgr::CreateInstance allocates 0x61C bytes and constructs; the game creates it once, at
// boot, and never destroys it.
extern "C" void ArchiveMgr_CreateInstance_8053fc4c(CpuContext* ctx) {
    CreateInstanceHook(ctx, Class(9).instanceGlobal, MKW_GUEST_NEXT(8053FC4C));
}
REGISTER_NATIVE_WRAPPER_AS(0x8053FC4C, ArchiveMgr_CreateInstance_8053fc4c, "ArchiveMgr_CreateInstance_8053fc4c");

// Item::GessoMgr::ApplyBlooper starts by clearing ink[0..11] with 12 unrolled stores; clear the
// windows of elements 12+ as well (a previous Blooper's ink would stay otherwise).
extern "C" void GessoMgr_ApplyBlooper_807a972c(CpuContext* ctx) {
    const ArrayClass& c = Class(7);
    for (uint32_t index = kInlineSlots; index < kMaxPlayers; ++index) {
        Memory::Write32(Window(c, index) + c.arraysBegin, 0);
    }
    MKW_GUEST_NEXT(807A972C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x807A972C, GessoMgr_ApplyBlooper_807a972c, "GessoMgr_ApplyBlooper_807a972c");
