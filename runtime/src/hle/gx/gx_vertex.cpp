// gx_vertex.cpp - Vertex Descriptor and Attribute Functions
#include "gx_internal.h"
#include "gx_stream_common.h"
#include "gx_cp_decode.h"
#include "fiber_manager.h"
#include "guest_interrupt_context.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
uint32_t CanonicalVtxAttr(uint32_t attr) {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// Array bases as last set through GXSetArray on the game thread.
std::array<std::array<uint32_t, 2>, 26> g_apiVtxArrays{};

// There is deliberately no indexed-aurora path here. The immediate begin path
// streams vertex data incrementally, and aurora's indexed-array upload must
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream below (GX_INDEX8/16 -> GX_DIRECT).
void ApplyAuroraVtxStateForBegin(GXVtxFmt fmt) {
    GxStream::PublishAuroraVtxState(fmt,
                                    GxStream::AuroraVtxPublishOptions{
                                        /*includeNbt=*/false,
                                        /*fmtLoopFirst=*/static_cast<int>(GX_VA_POS),
                                        /*fmtLoopLast=*/static_cast<int>(GX_VA_TEX7)});
}
}

bool ApiVtxArray(uint32_t attr, uint32_t& base, uint32_t& stride) {
    if (attr >= 26) return false;
    base = g_apiVtxArrays[attr][0];
    stride = g_apiVtxArrays[attr][1];
    return base != 0 && stride != 0;
}

// ============================================================================
// Vertex Descriptor
// ============================================================================

// The game thread's copy of the vertex descriptor, so GXGetVtxDesc does not always have to wait
// for the GX worker: g_hleGxState.vtxDesc belongs to the worker, and nw4r asks for it per draw.
// At 100 racers that wait was 19% of the game thread (762 of 4,010 samples over 10 s, M5 Pro),
// from about 2.6 million calls a race. The copy is exact right after a Sync, follows the
// Set/ClearVtxDesc calls the game thread records, and is marked unknown by anything that can
// change the descriptor from CP writes (FIFO writes, display lists: GxWorker::MarkVtxDescUnknown).
// Measured: 33% of the calls had a FIFO write since the previous one and 0.3% a display list, so
// about two thirds no longer wait.
// The vertex attribute formats get the same game-thread copy under the same flag: once
// GetVtxDesc stopped syncing, GetVtxAttrFmt became the call that waited (406 of the samples).
namespace {
GXAttrType s_gameVtxDesc[26] = {};
VtxAttrFmt s_gameVtxAttrFmt[8][26] = {};

void SetGameVtxDesc(uint32_t attr, uint32_t type, uint32_t rawAttr) {
    if (GxWorker::OnWorker() || attr >= 26 || attr == GX_VA_NULL) return;
    s_gameVtxDesc[attr] = static_cast<GXAttrType>(type);
    if (rawAttr == GX_VA_NBT) s_gameVtxDesc[GX_VA_NBT] = GX_NONE;
}

uint64_t s_vtxQueries = 0, s_vtxSyncs = 0, s_vtxMismatches = 0;

// MKW100_TEST_VTX_VERIFY: every query syncs and compares the copy with the worker's state (counted
// in the test profiler's vertex statistics, the first few logged).
bool VerifyGameVertexState() {
    static const bool verify = std::getenv("MKW100_TEST_VTX_VERIFY") != nullptr;
    return verify;
}

void SyncGameVertexState() {
    ++s_vtxQueries;
    if (!GxWorker::g_vtxDescUnknown && GxWorker::Active() && VerifyGameVertexState()) {
        GX_SYNC_INLINE();
        bool same = true;
        for (uint32_t i = 0; i < 26; ++i) same &= s_gameVtxDesc[i] == g_hleGxState.vtxDesc[i];
        for (uint32_t f = 0; f < 8; ++f)
            for (uint32_t i = 0; i < 26; ++i)
                same &= GxCpDecode::SameVtxAttrFmt(s_gameVtxAttrFmt[f][i], g_hleGxState.vtxAttrFmt[f][i]);
        if (!same && ++s_vtxMismatches <= 8) {
            std::printf("[gx] vertex state copy differs from the worker's (query %llu)\n",
                        static_cast<unsigned long long>(s_vtxQueries));
        }
        if (!same) GxWorker::g_vtxDescUnknown = true;
    }
    if (GxWorker::g_vtxDescUnknown || !GxWorker::Active()) {
        ++s_vtxSyncs;
        GX_SYNC_INLINE();
        for (uint32_t i = 0; i < 26; ++i) s_gameVtxDesc[i] = g_hleGxState.vtxDesc[i];
        std::memcpy(s_gameVtxAttrFmt, g_hleGxState.vtxAttrFmt, sizeof s_gameVtxAttrFmt);
        GxWorker::g_vtxDescUnknown = false;
    }
}

// The descriptor as the worker will see it at the next recorded command; game thread only.
const GXAttrType* GameVtxDesc() {
    SyncGameVertexState();
    return s_gameVtxDesc;
}

// Likewise the attribute formats.
const VtxAttrFmt (*GameVtxAttrFmt())[26] {
    SyncGameVertexState();
    return s_gameVtxAttrFmt;
}
} // namespace

// Test profiler statistics: game-thread vertex state queries, how many synced, why the copy went
// unknown, and (MKW100_TEST_VTX_VERIFY) how often it was wrong.
void GxVertexStats(uint64_t out[12]) {
    out[0] = s_vtxQueries; out[1] = s_vtxSyncs;
    for (int i = 0; i < 3; ++i) out[2 + i] = GxWorker::g_vtxMarks[i];
    out[5] = s_vtxMismatches;
    for (int i = 0; i < 3; ++i) out[6 + i] = GxWorker::g_vtxFollow[i];
    for (int i = 0; i < 3; ++i) out[9 + i] = GxWorker::g_vtxMarksEffective[i];
}

// The game thread's parse of the FIFO bytes it records ([video] gx_follow_layout). The HLE
// GXBegin is not in the byte stream; the vertex data that follows it is, and before this a float
// pair in it that read as "08 5x" made the next GXGetVtxDesc wait for everything queued so far:
// every particle emitter did, about 6.6 waits a frame and 11-14% of the game thread at 128 racers.
// The parser steps over vertex data (its size from the copy) and XF payloads, applies CP layout
// loads to the copy, and falls back to the conservative scan ("lost") wherever it cannot be sure of
// the command boundaries: until the next GXBegin (always at a boundary) after an unknown opcode, a
// draw while the copy is unknown, inline GX work (whose bytes it does not see) and display list
// recording.
bool GameVtxFollowDisplayListAt(uint32_t addr, uint32_t size);  // gx_dl.cpp

namespace {
struct GameFifoParser {
    bool lost = true;
    uint64_t skip = 0;     // vertex data / payload bytes to step over
    uint8_t cmd = 0;
    uint8_t buf[8] = {};
    uint32_t have = 0, need = 0;
} s_fifo;

// Test profiler statistics: why the parser lost the stream, and the effective marks it made.
enum { kLostInline, kLostDlRecord, kLostOpcode, kLostDrawUnknown, kLostBegin, kLostScanHit, kLostCount };
uint64_t s_fifoLost[kLostCount] = {};
uint64_t s_fifoOpcodes[256] = {};
void FifoLost(int why) {
    if (!s_fifo.lost) ++s_fifoLost[why];
    s_fifo.lost = true;
}

void GameFifoCommandDone() {
    auto& p = s_fifo;
    const uint8_t op = p.cmd & 0xF8;
    if (p.cmd == 0x08 || op == 0x08) {  // CP: register, value
        const uint8_t reg = p.buf[0];
        const uint32_t value = (uint32_t(p.buf[1]) << 24) | (uint32_t(p.buf[2]) << 16) | (uint32_t(p.buf[3]) << 8) | p.buf[4];
        if (!GxWorker::g_vtxDescUnknown) GxCpDecode::ApplyCpLayoutWriteInto(s_gameVtxDesc, s_gameVtxAttrFmt, reg, value);
    } else if (op == 0x10) {  // XF: count - 1, address, then the data
        p.skip = (((uint32_t(p.buf[0]) << 8) | p.buf[1]) + 1u) * 4u;
    } else if (p.cmd >= 0x80 && p.cmd < 0xC0) {  // draw: vertex count, then the vertices
        if (GxWorker::g_vtxDescUnknown) {
            FifoLost(kLostDrawUnknown);
        } else {
            const uint32_t count = (uint32_t(p.buf[0]) << 8) | p.buf[1];
            p.skip = uint64_t(count) * GxCpDecode::LayoutVertexSize(s_gameVtxDesc, s_gameVtxAttrFmt[p.cmd & 7]);
        }
    } else if (op == 0x40) {  // a display list called from the FIFO: address, size
        const uint32_t addr = (uint32_t(p.buf[0]) << 24) | (uint32_t(p.buf[1]) << 16) | (uint32_t(p.buf[2]) << 8) | p.buf[3];
        const uint32_t size = (uint32_t(p.buf[4]) << 24) | (uint32_t(p.buf[5]) << 16) | (uint32_t(p.buf[6]) << 8) | p.buf[7];
        if (!GxWorker::g_vtxDescUnknown && !GameVtxFollowDisplayListAt(addr, size)) GxWorker::MarkVtxDescUnknown(2);
    }
    p.need = p.have = 0;
}
} // namespace

void GameFifoBytes(const uint8_t* data, size_t size) {
    auto& p = s_fifo;
    if (GxWorker::detail::t_inlineDepth != 0) FifoLost(kLostInline);
    if (IsDisplayListActive()) FifoLost(kLostDlRecord);
    while (size != 0) {
        if (p.lost) {
            const bool known = !GxWorker::g_vtxDescUnknown;
            GxWorker::ScanFifoBytes(data, size);
            if (known && GxWorker::g_vtxDescUnknown) ++s_fifoLost[kLostScanHit];
            return;
        }
        if (p.skip != 0) {
            const uint64_t n = p.skip < size ? p.skip : size;
            p.skip -= n;
            data += n;
            size -= size_t(n);
            continue;
        }
        if (p.need != 0) {
            while (size != 0 && p.have < p.need) {
                p.buf[p.have++] = *data++;
                --size;
            }
            if (p.have == p.need) GameFifoCommandDone();
            continue;
        }
        const uint8_t c = *data++;
        --size;
        const uint8_t op = c & 0xF8;
        p.cmd = c;
        if (c == 0x00 || op == 0x48) continue;            // NOP, invalidate vertex cache
        if (c == 0x61) { p.skip = 4; continue; }          // BP
        if (op == 0x08) { p.need = 5; continue; }         // CP
        if (op == 0x10) { p.need = 4; continue; }         // XF header
        if (op >= 0x20 && op <= 0x38) { p.skip = 4; continue; }  // indexed XF
        if (op == 0x40) { p.need = 8; continue; }         // call display list
        if (c >= 0x80 && c < 0xC0) { p.need = 2; continue; }      // draw
        ++s_fifoOpcodes[c];
        FifoLost(kLostOpcode);                            // not a command: stop trusting the stream
        GxWorker::MarkVtxDescUnknown(1);
    }
}

// A GXBegin on the game thread: a command boundary, and the vertex data that follows is stepped
// over when the copy knows the vertex size.
void GameVtxNoteBegin(uint32_t vf, uint32_t nv) {
    auto& p = s_fifo;
    p.need = p.have = 0;
    p.skip = 0;
    p.lost = false;
    if (vf >= 8 || GxWorker::g_vtxDescUnknown || !GxWorker::Active() || !GxFollowLayoutLoads() ||
        GxWorker::detail::t_inlineDepth != 0)
        FifoLost(kLostBegin);
    if (!p.lost) p.skip = uint64_t(nv) * GxCpDecode::LayoutVertexSize(s_gameVtxDesc, s_gameVtxAttrFmt[vf]);
}

void GxFifoParserStats() {
    std::printf("[race-perf] FIFO parse lost: inline %llu, display list recording %llu, bad opcode %llu, draw while unknown %llu, "
                "begin while unknown %llu; conservative-scan marks %llu; bad opcodes:",
                (unsigned long long)s_fifoLost[0], (unsigned long long)s_fifoLost[1], (unsigned long long)s_fifoLost[2],
                (unsigned long long)s_fifoLost[3], (unsigned long long)s_fifoLost[4], (unsigned long long)s_fifoLost[5]);
    for (int i = 0; i < 256; ++i)
        if (s_fifoOpcodes[i]) std::printf(" %02x:%llu", i, (unsigned long long)s_fifoOpcodes[i]);
    std::printf("%c", 10);
}

// The game thread's copy for gx_dl.cpp, which follows the CP layout loads of display lists in it
// (GameVtxApplyDisplayList): null while the copy is unknown, or off the game thread.
bool GameVtxShadow(GXAttrType*& desc, VtxAttrFmt (*&fmts)[26]) {
    if (GxWorker::OnWorker() || GxWorker::g_vtxDescUnknown || !GxWorker::Active()) return false;
    desc = s_gameVtxDesc;
    fmts = s_gameVtxAttrFmt;
    return true;
}

extern "C" void GX__ClearVtxDesc_8016dc34() {
    if (!GxWorker::OnWorker()) for (auto& type : s_gameVtxDesc) type = GX_NONE;
    GX_DEFER_CALL0(GX__ClearVtxDesc_8016dc34);
    bool changed = false;
    for(int i=0; i<26; ++i){
        changed |= g_hleGxState.vtxDesc[i] != GX_NONE;
        g_hleGxState.vtxDesc[i]=GX_NONE;
    }
    if (changed) g_hleGxState.InvalidateVtxLayoutHash();
    // GXClearVtxDesc resets descriptors only; array base/stride state persists.
    GXClearVtxDesc();
}
PPC_NATIVE_OVERRIDE_VOID(8016dc34, GX__ClearVtxDesc_8016dc34, (), ());

extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t) {
    SetGameVtxDesc(CanonicalVtxAttr(a), t, a);
    GX_DEFER_CALL(GX__SetVtxDesc_8016d3a4, a, t);
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr>=26||attr==GX_VA_NULL) return;
    const GXAttrType oldType = g_hleGxState.vtxDesc[attr];
    g_hleGxState.vtxDesc[attr]=(GXAttrType)t;
    if (a == GX_VA_NBT) {
        g_hleGxState.vtxDesc[GX_VA_NBT] = GX_NONE;
    }
    if (g_hleGxState.vtxDesc[attr] != oldType) g_hleGxState.InvalidateVtxLayoutHash();
    if(IsMatrixIndexAttr((GXAttr)attr)) return;
    GXSetVtxDesc((GXAttr)a, (t==GX_INDEX8||t==GX_INDEX16)?GX_DIRECT:(GXAttrType)t);
}
PPC_NATIVE_OVERRIDE_VOID(8016d3a4, GX__SetVtxDesc_8016d3a4, (uint32_t a, uint32_t t), (a, t));

extern "C" void GX__SetVtxDescv_8016d608(uint32_t la) {
    if(!la) return; uint32_t p=la;
    std::vector<std::array<uint32_t, 2>> list;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        list.push_back({a, Memory::Read32(p+4)});
        SetGameVtxDesc(CanonicalVtxAttr(a), Memory::Read32(p+4), a);
        p+=8;
    }
    GxWorker::Run([list = std::move(list)] {
        for (const auto& [a, t] : list) GX__SetVtxDesc_8016d3a4(a, t);
    });
}
PPC_NATIVE_OVERRIDE_VOID(8016d608, GX__SetVtxDescv_8016d608, (uint32_t la), (la));

// Display lists can change the vertex layout; GameVtxDesc() syncs whenever one may have.
extern "C" void GX__GetVtxDesc_8016d9f0(uint32_t a, uint32_t tp) {
    GXAttrType type = GX_NONE;
    const uint32_t attr = CanonicalVtxAttr(a);
    if (attr < 26 && attr != GX_VA_NULL) {
        type = GxWorker::OnWorker() ? g_hleGxState.vtxDesc[attr] : GameVtxDesc()[attr];
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(type));
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016d9f0, GX__GetVtxDesc_8016d9f0, (uint32_t a, uint32_t tp), (a, tp));

extern "C" void GX__GetVtxDescv_8016dba4(uint32_t la) {
    if (!la) {
        return;
    }
    const GXAttrType* desc = GxWorker::OnWorker() ? g_hleGxState.vtxDesc : GameVtxDesc();
    uint32_t p = la;
    for (uint32_t a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(desc[a]));
        p += 8;
    }
    Memory::Write32(p, GX_VA_NBT);
    Memory::Write32(p + 4, static_cast<uint32_t>(desc[GX_VA_NRM]));
    p += 8;
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8016dba4, GX__GetVtxDescv_8016dba4, (uint32_t la), (la));

// ============================================================================
// Vertex Attribute Format
// ============================================================================

extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if (!GxWorker::OnWorker() && vf < 8 && attr < 26) {
        s_gameVtxAttrFmt[vf][attr] = {(GXCompCnt)c, (GXCompType)t, (u8)fr};
        if (a == GX_VA_NBT) s_gameVtxAttrFmt[vf][GX_VA_NBT] = {};
    }
    GX_DEFER_CALL(GX__SetVtxAttrFmt_8016dc68, vf, a, c, t, fr);
    if(vf<8&&attr<26){
        const VtxAttrFmt oldFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        g_hleGxState.vtxAttrFmt[vf][attr].cnt=(GXCompCnt)c;
        g_hleGxState.vtxAttrFmt[vf][attr].type=(GXCompType)t;
        g_hleGxState.vtxAttrFmt[vf][attr].frac=(u8)fr;
        if (a == GX_VA_NBT) {
            g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT] = {};
        }
        const auto& newFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        if (oldFmt.cnt != newFmt.cnt || oldFmt.type != newFmt.type || oldFmt.frac != newFmt.frac) {
            g_hleGxState.InvalidateVtxLayoutHash();
        }
    }
    if (vf >= GX_MAX_VTXFMT || a < GX_VA_POS || a >= GX_VA_MAX_ATTR) {
        return;
    }
    GXSetVtxAttrFmt((GXVtxFmt)vf, (GXAttr)a, (GXCompCnt)c, (GXCompType)t, (u8)fr);
}
PPC_NATIVE_OVERRIDE_VOID(8016dc68, GX__SetVtxAttrFmt_8016dc68, (uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr), (vf, a, c, t, fr));

extern "C" void GX__SetVtxAttrFmtv_8016de08(uint32_t vf, uint32_t la) {
    if(!la) return; uint32_t p=la;
    std::vector<std::array<uint32_t, 4>> list;
    while(true){
        uint32_t a=Memory::Read32(p);
        if(a==0xFFu) break;
        list.push_back({a, Memory::Read32(p+4), Memory::Read32(p+8), Memory::Read8(p+12)});
        p+=16;
    }
    GxWorker::MarkVtxDescUnknown();  // applied on the worker, past the game-thread copy
    GxWorker::Run([vf, list = std::move(list)] {
        for (const auto& [a, c, t, fr] : list) GX__SetVtxAttrFmt_8016dc68(vf, a, c, t, fr);
    });
}
PPC_NATIVE_OVERRIDE_VOID(8016de08, GX__SetVtxAttrFmtv_8016de08, (uint32_t vf, uint32_t la), (vf, la));

extern "C" void GX__GetVtxAttrFmt_8016e04c(uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp) {
    VtxAttrFmt fmt{};
    const uint32_t attr = CanonicalVtxAttr(a);
    if (vf < 8 && attr < GX_VA_MAX_ATTR && attr != GX_VA_NULL) {
        fmt = GameVtxAttrFmt()[vf][attr];
    }
    if (cp) {
        Memory::Write32(cp, static_cast<uint32_t>(fmt.cnt));
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(fmt.type));
    }
    if (fp) {
        Memory::Write8(fp, fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016e04c, GX__GetVtxAttrFmt_8016e04c, (uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp), (vf, a, cp, tp, fp));

extern "C" void GX__GetVtxAttrFmtv_8016e2b8(uint32_t vf, uint32_t la) {
    if (!la) {
        return;
    }
    const auto formats = GameVtxAttrFmt();
    uint32_t p = la;
    for (uint32_t a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
        VtxAttrFmt fmt{};
        if (vf < 8) {
            fmt = formats[vf][a];
        }
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(fmt.cnt));
        Memory::Write32(p + 8, static_cast<uint32_t>(fmt.type));
        Memory::Write8(p + 12, fmt.frac);
        p += 16;
    }
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8016e2b8, GX__GetVtxAttrFmtv_8016e2b8, (uint32_t vf, uint32_t la), (vf, la));

// ============================================================================
// Vertex Arrays
// ============================================================================

extern "C" void GX__SetArray_8016e32c(uint32_t a, uint32_t ba, uint32_t str) {
    if (!GxWorker::OnWorker()) {
        const uint32_t attr = CanonicalVtxAttr(a);
        if (attr < 26) g_apiVtxArrays[attr] = {ba, str};
    }
    GX_DEFER_CALL(GX__SetArray_8016e32c, a, ba, str);
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr<26){ g_hleGxState.vtxArray[attr].base=ba; g_hleGxState.vtxArray[attr].stride=str; }
}
PPC_NATIVE_OVERRIDE_VOID(8016e32c, GX__SetArray_8016e32c, (uint32_t a, uint32_t ba, uint32_t str), (a, ba, str));

// Switch-artifact entry point for the same SDK function; forwards rather than
// repeating the body.
extern "C" void GX__SetArray_8016e1c4(uint32_t a, uint32_t b, uint32_t s) { GX__SetArray_8016e32c(a, b, s); }
PPC_NATIVE_OVERRIDE_VOID(8016e1c4, GX__SetArray_8016e1c4, (uint32_t a, uint32_t b, uint32_t s), (a, b, s));

// ============================================================================
// Texture Coordinate Generation
// ============================================================================

extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t n) {
    GX_DEFER_CALL(GX__SetNumTexGens_8016e5a4, n);
    GXSetNumTexGens((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(8016e5a4, GX__SetNumTexGens_8016e5a4, (uint32_t n), (n));

extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm) {
    GX_DEFER_CALL(GX__SetTexCoordGen2_8016e37c, dc, f, sp, m, n, pm);
    if (sp >= static_cast<uint32_t>(GX_MAX_TEXGENSRC)) {
        uint32_t pc = 0;
        uint32_t lr = 0;
        if (auto* cpu = TryGetCpuContext()) {
            pc = cpu->pc;
            lr = cpu->lr;
        }
        RT_LOGF(RT_TAG_GX,
                "GXSetTexCoordGen2 invalid src=%u dst=%u type=%u mtx=%u norm=%u post=%u PC=0x%08X LR=0x%08X\n",
                sp, dc, f, m, n, pm, pc, lr);
    }
    GXSetTexCoordGen2((GXTexCoordID)dc, (GXTexGenType)f, (GXTexGenSrc)sp, m, (GXBool)n, pm);
}
PPC_NATIVE_OVERRIDE_VOID(8016e37c, GX__SetTexCoordGen2_8016e37c, (uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm), (dc, f, sp, m, n, pm));

extern "C" void GX__EnableTexOffsets_8016f37c(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable) {
    GX_DEFER_CALL(GX__EnableTexOffsets_8016f37c, coord, lineEnable, pointEnable);
    GXEnableTexOffsets(static_cast<GXTexCoordID>(coord),
                       lineEnable ? GX_TRUE : GX_FALSE,
                       pointEnable ? GX_TRUE : GX_FALSE);
}
PPC_NATIVE_OVERRIDE_VOID(8016f37c, GX__EnableTexOffsets_8016f37c, (uint32_t coord, uint32_t lineEnable, uint32_t pointEnable), (coord, lineEnable, pointEnable));

extern "C" void GX__SetLineWidth_8016f314(uint32_t width, uint32_t texOffsets) {
    GX_DEFER_CALL(GX__SetLineWidth_8016f314, width, texOffsets);
    GXSetLineWidth(static_cast<u8>(width), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8016f314, GX__SetLineWidth_8016f314, (uint32_t width, uint32_t texOffsets), (width, texOffsets));

extern "C" void GX__SetPointSize_8016f348(uint32_t pointSize, uint32_t texOffsets) {
    GX_DEFER_CALL(GX__SetPointSize_8016f348, pointSize, texOffsets);
    GXSetPointSize(static_cast<u8>(pointSize), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8016f348, GX__SetPointSize_8016f348, (uint32_t pointSize, uint32_t texOffsets), (pointSize, texOffsets));

// ============================================================================
// Begin/End Drawing
// ============================================================================

namespace {
thread_local bool g_inGxBeginOrFifo = false;
thread_local std::chrono::steady_clock::time_point g_nextDeferredTimingPoll{};

void ServiceDeferredTimingDuringGxWork() {
    if (g_inGxBeginOrFifo) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextDeferredTimingPoll) {
        return;
    }
    g_nextDeferredTimingPoll = now + std::chrono::milliseconds(1);

    g_inGxBeginOrFifo = true;
    try {
        if (Fiber::GuestFiberManager::IsInitialized()) {
            // VI callbacks are what advance EGG::AsyncDisplay on NTSC/PAL60;
            // PAL50 uses an OS alarm. Keep both clocks moving while a slow
            // host frame is still being submitted, but defer fiber switches
            // and Aurora work until the native GX call has unwound.
            VI_HLE_ProcessRetracesDeferred(8);
            OS_HLE_ProcessAlarmsDeferred(8);
        } else if (TryGetCpuContext() != nullptr) {
            // Same interrupt isolation as the deferred paths above: this GX call
            // is mid-execution of a translated function, so the retrace
            // callbacks must not publish their registers into its context.
            GuestInterruptCallbackContext interrupt;
            VI_HLE_PollRetrace(interrupt.get());
        }
    } catch (...) {
        g_inGxBeginOrFifo = false;
        throw;
    }
    g_inGxBeginOrFifo = false;
}
}

static void GxBeginImpl(uint32_t t, uint32_t vf, uint32_t nv);

extern "C" void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv) {
    if(IsDisplayListActive()){
        WriteDisplayListData((u8)(t|vf), 1);
        WriteDisplayListData((u16)nv, 2);
        return;
    }
    ServiceDeferredTimingDuringGxWork();
    if (!GxWorker::OnWorker()) GameVtxNoteBegin(vf, nv);
    GX_DEFER_CALL(GxBeginImpl, t, vf, nv);
    GxBeginImpl(t, vf, nv);
}

static void GxBeginImpl(uint32_t t, uint32_t vf, uint32_t nv) {
    ApplyAuroraVtxStateForBegin(static_cast<GXVtxFmt>(vf));
    g_hleGxState.currentVtxFmt=(GXVtxFmt)vf; g_hleGxState.currentPrim=(GXPrimitive)t;
    g_hleGxState.vertsRemaining=nv; g_hleGxState.inBegin=nv!=0; g_hleGxState.auroraBeginCalled=false;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount=0; g_hleGxState.ResetVertex();
}
PPC_NATIVE_OVERRIDE_VOID(8016f0f0, GX__Begin_8016f0f0, (uint32_t t, uint32_t vf, uint32_t nv), (t, vf, nv));

extern "C" void GX__End_80044b30() {
    GX_DEFER_CALL0(GX__End_80044b30); g_hleGxState.inBegin=false; GXEnd(); }
PPC_NATIVE_OVERRIDE_VOID(80044b30, GX__End_80044b30, (), ());

extern "C" void GX__End_80048c30() { GX__End_80044b30(); }
PPC_NATIVE_OVERRIDE_VOID(80048c30, GX__End_80048c30, (), ());

extern "C" void GX__DrawSphere_80172a30(uint32_t numMajor, uint32_t numMinor) {
    GxWorker::MarkVtxDescUnknown();  // rewrites g_hleGxState.vtxDesc directly
    if (GxWorker::ShouldDefer()) {
        EnsureAuroraFrameActive();
        GxWorker::Submit([=] { GX__DrawSphere_80172a30(numMajor, numMinor); });
        return;
    }
    constexpr uint32_t kAttrCount = 26;
    constexpr float kSphereRadius = 1.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;

    std::array<GXAttrType, kAttrCount> savedVtxDesc{};
    std::array<VtxAttrFmt, kAttrCount> savedFmt3{};
    for (uint32_t i = 0; i < kAttrCount; ++i) {
        savedVtxDesc[i] = g_hleGxState.vtxDesc[i];
        savedFmt3[i] = g_hleGxState.vtxAttrFmt[GX_VTXFMT3][i];
    }

    const bool hadTex0 = savedVtxDesc[GX_VA_TEX0] != GX_NONE;

    bool drawOpen = false;
    try {
        EnsureAuroraFrameActive();

        for (uint32_t i = 0; i < kAttrCount; ++i) {
            g_hleGxState.vtxDesc[i] = GX_NONE;
        }
        g_hleGxState.InvalidateVtxLayoutHash();
        GXClearVtxDesc();

        GX__SetVtxDesc_8016d3a4(GX_VA_POS, GX_DIRECT);
        GX__SetVtxDesc_8016d3a4(GX_VA_NRM, GX_DIRECT);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        if (hadTex0) {
            GX__SetVtxDesc_8016d3a4(GX_VA_TEX0, GX_DIRECT);
            GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        }

        const float majorStep = kPi / static_cast<float>(numMajor);
        const float minorStep = kTwoPi / static_cast<float>(numMinor);
        const float invRadius = 1.0f / kSphereRadius;

        for (uint32_t major = 0; major < numMajor; ++major) {
            const float major0 = static_cast<float>(major) * majorStep;
            const float major1 = major0 + majorStep;

            const float ring0 = kSphereRadius * std::sin(major0);
            const float ring1 = kSphereRadius * std::sin(major1);
            const float z0 = kSphereRadius * std::cos(major0);
            const float z1 = kSphereRadius * std::cos(major1);

            const uint16_t vertexCount = static_cast<uint16_t>(((numMinor + 1u) * 2u) & 0xFFFEu);
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, vertexCount);
            drawOpen = true;

            const float nz0 = z0 * invRadius;
            const float nz1 = z1 * invRadius;

            for (uint32_t minor = 0; minor <= numMinor; ++minor) {
                const float minorAngle = static_cast<float>(minor) * minorStep;
                const float c = std::cos(minorAngle);
                const float s = std::sin(minorAngle);

                const float x1 = c * ring1;
                const float y1 = s * ring1;
                GXPosition3f32(x1, y1, z1);
                GXNormal3f32(x1 * invRadius, y1 * invRadius, nz1);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major + 1u) / static_cast<float>(numMajor));
                }

                const float x0 = c * ring0;
                const float y0 = s * ring0;
                GXPosition3f32(x0, y0, z0);
                GXNormal3f32(x0 * invRadius, y0 * invRadius, nz0);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major) / static_cast<float>(numMajor));
                }
            }
            GXEnd();
            drawOpen = false;
        }
    } catch (...) {
        if (drawOpen) {
            GXEnd();
            drawOpen = false;
        }
    }

    for (uint32_t i = 0; i < kAttrCount; ++i) {
        g_hleGxState.vtxDesc[i] = GX_NONE;
    }
    g_hleGxState.InvalidateVtxLayoutHash();
    GXClearVtxDesc();
    for (uint32_t attr = 0; attr < kAttrCount; ++attr) {
        const GXAttrType type = savedVtxDesc[attr];
        if (type != GX_NONE) {
            GX__SetVtxDesc_8016d3a4(attr, static_cast<uint32_t>(type));
        }
    }
    for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
        const VtxAttrFmt& fmt = savedFmt3[attr];
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, attr, static_cast<uint32_t>(fmt.cnt), static_cast<uint32_t>(fmt.type), fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80172a30, GX__DrawSphere_80172a30, (uint32_t numMajor, uint32_t numMinor), (numMajor, numMinor));
