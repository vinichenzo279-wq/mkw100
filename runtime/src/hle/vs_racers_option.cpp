// mkw100: a "Racers" row on the VS rules page, below "Number of Races", and the same on the battle
// rules page (Pages::BattleSettings, ctor 0x80629184, the same base with its own copies of the
// handlers, string table and layout names "BattleSetting...": 4 radio rows and the round count at
// id 4, so the new row is id 5; its help line is message 3466).
//
// Pages::VSSettings (page 0x73, ctor 0x806284C8) builds 6 controls through CreateControl
// (0x808530B8): 5 radio rows and, for id 5, the race count UpDownControl. Its count lives at
// page +0x3D8. Here the count becomes 7 and id 6 is built like id 5, from the same layout files
// (VSSettingUpDownBase variant "UpDownRaceNum" and its buttons and value text), one row lower
// (rows are 40 units apart: race count at y -60, the new row at -100). The OK button moves down
// to stay clear of it. Nothing from the game is copied: the controls load the game's own files at
// runtime.
//
// Its label is the race count's own message (BMG 3450) swapped for "Racers" while the row loads,
// by answering that one lookup from a runtime string (BMGHolder::GetMsgId 0x805F8C88,
// GetMsgByMsgId 0x805F8CF0, GetFontIndex 0x805F8D2C). Its value uses message 3451, the number
// the race count shows; its help line swaps 3465 the same way. Changes go to LocalVs::SetDesiredCount, the setting F10 > Local VS edits.

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "local_vs.h"
#include "hle/cc200.h"
#include "hle/custom_messages.h"
#include "memory.h"
#include "runtime_log.h"

#include <array>
#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kVsControlCount = 0x3D8;      // page: internal control count, 6
constexpr uint32_t kVsControlCursor = 0x654;     // page: next control index for AddControl

constexpr uint32_t kOperatorNew = 0x80229DCC;
constexpr uint32_t kAddControl = 0x8060246C;     // MenuInteractable: page, index, control, 0
constexpr uint32_t kSprintf = 0x80011938;        // snprintf(buffer, size, format, ...)
constexpr uint32_t kUpDownCtor = 0x8063E960;
constexpr uint32_t kUpDownLoad = 0x8063EBDC;
constexpr uint32_t kUpDownSize = 1480;
constexpr uint32_t kUpDownId = 0x20C;            // the page's handlers read the row id here
constexpr uint32_t kUpDownHandlerFields = 0x29C; // copies of the three handler pointers
constexpr uint32_t kTextCtor = 0x806405D0;
constexpr uint32_t kTextLoad = 0x806406B0;
constexpr uint32_t kTextSize = 1140;
constexpr uint32_t kTextInner = 0x174;           // the TextControl the UpDown displays through
constexpr uint32_t kTextHandlerField = 0x178;
constexpr uint32_t kTextInfoInit = 0x805CD94C;
constexpr uint32_t kSetMessage = 0x8063DDB4;     // LayoutUIControl: control, bmg id, Text::Info*

constexpr uint32_t kLabelBmg = 3450;             // "Number of Races"
constexpr uint32_t kValueBmg = 3451;             // the number in Text::Info intToPass[0]
constexpr uint32_t kRacersValueSlot = 20;        // CustomMessages::LabelBmg slot
constexpr uint32_t kInjectedMsg = 0x7FFF;        // msg index no BMG file reaches
constexpr uint32_t kHelpMsg = 0x7FFE;
constexpr float kRowStep = 20.0f;                // measured: 40 put the row two rows below Races
constexpr float kOkDrop = 12.0f;
constexpr float kOkScaleY = 0.85f;               // with the drop, keeps OK clear of both the row and the help bar

// "Racers" as UTF-16BE in the fixed runtime table span. Its INF entry (font and attributes) is
// the race count label's own: a zeroed entry selected a caps-only font.
constexpr uint32_t kLabelText = ArrayOverflow::kMenuText;
constexpr char kLabel[] = "Racers";
constexpr uint32_t kHelpText = kLabelText + 0x40;
constexpr char kHelp[] = "Set the number of racers.";

// The page the row goes on; the two rules pages differ only in these.
struct RacersPage {
    uint32_t ctor, strings, variants, variantOffset, upDownHandlers, textHandler, rowId, helpBmg;
};
constexpr RacersPage kVsPage{0x806284C8, 0x808AE874, 0x808AE7E8, 128, 0x1E00, 0x1E0C, 6, 3465};
constexpr RacersPage kBattlePage{0x80629184, 0x808ACE44, 0x808ACDD8, 96, 0x1B68, 0x1B74, 5, 3466};
// String offsets in each table (the same names, "VSSetting..." / "BattleSetting...").
struct RacersStrings { uint32_t format, control, base, buttonR, right, buttonL, left, textControl, value, valueName, text, textName; };
constexpr RacersStrings kVsStringsAt{93, 102, 110, 130, 153, 165, 188, 199, 207, 228, 234, 254};
constexpr RacersStrings kBattleStringsAt{105, 114, 122, 146, 173, 185, 212, 223, 231, 256, 262, 286};

uint32_t gHelpBmg = 3465;

bool gLoadingRow = false;     // game thread only: the row's layout is loading
uint32_t gLabelMsg = 0;       // BMG msg index of kLabelBmg, for its INF entry
bool gSettingHelp = false;    // game thread only: the row's help text is being set
uint32_t gHelpMsgIndex = 0;
// The row built on each rules page (VS and battle pages, single and multiplayer sections all exist
// at once): handlers get their page in r3 and look its row up here. Game thread only.
struct RacersRow { uint32_t page, row, text; };
std::array<RacersRow, 8> gRows{};
RacersRow* RowOf(uint32_t page) {
    for (auto& r : gRows) if (r.page == page && page != 0) return &r;
    return nullptr;
}
void ForgetPage(uint32_t page) {
    for (auto& r : gRows) if (r.page == page) r = {};
}
void RememberRow(uint32_t page, uint32_t row, uint32_t text) {
    ForgetPage(page);
    for (auto& r : gRows) if (r.page == 0) { r = {page, row, text}; return; }
    for (size_t i = 1; i < gRows.size(); ++i) gRows[i - 1] = gRows[i];  // full: drop the oldest
    gRows.back() = {page, row, text};
}
bool IsRow(uint32_t page, uint32_t control) { const RacersRow* r = RowOf(page); return r && control == r->row; }
// The handler gets the value's inner TextControl (text +0x17C), so match the whole object.
bool IsRowText(uint32_t page, uint32_t control) { const RacersRow* r = RowOf(page); return r && control - r->text < kTextSize; }

void WriteLabel() {
    for (uint32_t i = 0; i < sizeof kLabel; ++i) Memory::Write16(kLabelText + i * 2, uint8_t(kLabel[i]));
    for (uint32_t i = 0; i < sizeof kHelp; ++i) Memory::Write16(kHelpText + i * 2, uint8_t(kHelp[i]));
}

uint32_t Call(CpuContext* ctx, uint32_t function) {
    InvokeIndirectCpu(function, ctx);
    return ctx->gpr[3];
}

void ShiftY(uint32_t control, float dy) {
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t y = control + 4 + i * 0x18 + 4;
        Memory::WriteFloat32(y, Memory::ReadFloat32(y) + dy);
    }
}

// The arrow buttons and the value text are children (UIControl childrenGroup at +0x68: array at
// +0, count at +0x10) with their own positions, so they move with the row explicitly.
void ShiftTree(uint32_t control, float dy) {
    ShiftY(control, dy);
    const uint32_t children = Memory::Read32(control + 0x68);
    const uint32_t count = Memory::Read32(control + 0x78);
    for (uint32_t i = 0; children != 0 && i < count && i < 8; ++i) {
        const uint32_t child = Memory::Read32(children + i * 4);
        if (child != 0) ShiftTree(child, dy);
    }
}

uint32_t BuildRow(CpuContext* ctx, uint32_t page, const RacersPage& kind) {
    const RacersStrings& at = &kind == &kBattlePage ? kBattleStringsAt : kVsStringsAt;
    const uint32_t strings = kind.strings;
    const CpuContext entry = *ctx;
    const uint32_t sp = entry.gpr[1] - 0x80;
    Memory::Write32(sp, entry.gpr[1]);
    ctx->gpr[1] = sp;
    const CpuContext base = *ctx;

    *ctx = base; ctx->gpr[3] = kUpDownSize; const uint32_t row = Call(ctx, kOperatorNew);
    *ctx = base; ctx->gpr[3] = kTextSize; const uint32_t text = Call(ctx, kOperatorNew);
    if (row == 0 || text == 0) { *ctx = entry; return 0; }
    *ctx = base; ctx->gpr[3] = row; Call(ctx, kUpDownCtor);
    *ctx = base; ctx->gpr[3] = text; Call(ctx, kTextCtor);
    // Set before Load: Load already fires the change handlers with the initial option.
    RememberRow(page, row, text);

    const uint32_t index = Memory::Read32(page + kVsControlCursor);
    Memory::Write32(page + kVsControlCursor, index + 1);
    *ctx = base; ctx->gpr[3] = page; ctx->gpr[4] = index; ctx->gpr[5] = row; ctx->gpr[6] = 0;
    Call(ctx, kAddControl);

    // Variant name "UpDownRaceNum" in the frame's scratch area.
    const uint32_t variant = sp + 0x40;
    *ctx = base; ctx->gpr[3] = variant; ctx->gpr[4] = 16; ctx->gpr[5] = strings + at.format;
    ctx->gpr[6] = Memory::Read32(kind.variants + kind.variantOffset); ctx->cr &= ~(1u << (31 - 6));
    Call(ctx, kSprintf);

    const uint32_t count = uint32_t(LocalVs::MaxRacers());
    *ctx = base;
    ctx->gpr[3] = row; ctx->gpr[4] = count; ctx->gpr[5] = uint32_t(LocalVs::DesiredCount() - 1);
    ctx->gpr[6] = strings + at.control; ctx->gpr[7] = strings + at.base; ctx->gpr[8] = variant;
    ctx->gpr[9] = strings + at.buttonR; ctx->gpr[10] = strings + at.right;
    Memory::Write32(sp + 8, strings + at.buttonL); Memory::Write32(sp + 12, strings + at.left);
    Memory::Write32(sp + 16, text + kTextInner); Memory::Write32(sp + 20, 1);
    Memory::Write32(sp + 24, 0); Memory::Write32(sp + 28, 0);
    Memory::Write32(sp + 32, 1); Memory::Write32(sp + 36, 1);
    WriteLabel();
    gLoadingRow = true;
    Call(ctx, kUpDownLoad);
    gLoadingRow = false;
    for (uint32_t k = 0; k < 3; ++k) {
        Memory::Write32(row + kUpDownHandlerFields + k * 4, Memory::Read32(page + kind.upDownHandlers + k * 4));
    }
    Memory::Write32(row + kUpDownId, 1);

    *ctx = base;
    ctx->gpr[3] = text; ctx->gpr[4] = strings + at.textControl; ctx->gpr[5] = strings + at.value;
    ctx->gpr[6] = strings + at.valueName; ctx->gpr[7] = strings + at.text; ctx->gpr[8] = strings + at.textName;
    Call(ctx, kTextLoad);
    Memory::Write32(text + kTextHandlerField, Memory::Read32(page + kind.textHandler));

    ShiftTree(row, -kRowStep);
    RT_LOGF("vs-racers", "racers row built: control %08X, %u options, value %d\n", row, count, LocalVs::DesiredCount());
    *ctx = entry;
    return row;
}

void SetRowValue(CpuContext* ctx, uint32_t textControl, uint32_t option) {
    const CpuContext entry = *ctx;
    const uint32_t sp = entry.gpr[1] - 0x100;
    Memory::Write32(sp, entry.gpr[1]);
    ctx->gpr[1] = sp;
    const uint32_t info = sp + 0x10;
    ctx->gpr[3] = info; Call(ctx, kTextInfoInit);
    Memory::Write32(info, option + 1);
    // The race count's value message reads "Races: N"; this row says "Racers: N" (a runtime label).
    const uint32_t value = CustomMessages::LabelBmg(kRacersValueSlot, "Racers: " + std::to_string(option + 1), kValueBmg);
    ctx->gpr[1] = sp; ctx->gpr[3] = textControl; ctx->gpr[4] = value ? value : kValueBmg; ctx->gpr[5] = info;
    Call(ctx, kSetMessage);
    *ctx = entry;
}

} // namespace

extern "C" void VsSettings_Ctor_806284c8(CpuContext* ctx) {
    const uint32_t page = ctx->gpr[3];
    MKW_GUEST_NEXT(806284C8)(ctx);
    Memory::Write32(page + kVsControlCount, kVsPage.rowId + 1);
    ForgetPage(page);
}

extern "C" void BattleSettings_Ctor_80629184(CpuContext* ctx) {
    const uint32_t page = ctx->gpr[3];
    MKW_GUEST_NEXT(80629184)(ctx);
    Memory::Write32(page + kVsControlCount, kBattlePage.rowId + 1);
    ForgetPage(page);
}
REGISTER_NATIVE_WRAPPER_AS(0x80629184, BattleSettings_Ctor_80629184, "BattleSettings_Ctor_80629184");
REGISTER_NATIVE_WRAPPER_AS(0x806284C8, VsSettings_Ctor_806284c8, "VsSettings_Ctor_806284c8");

// The Class row offers Mirror only when the license's completion bit 52 is set (an inlined
// RKSYS::LicenseCompletion::IsCompleted). With the unlocks setting it is set for the row's build
// and put back after, so nothing reaches the save.
constexpr uint32_t kSaveManager = 0x809BD748;  // SaveDataManager instance (current license +0x36)
constexpr uint32_t kLicenseBase = 0x38, kLicenseSize = 0x93F0, kMirrorWord = 0x9000, kMirrorBit = 1u << 20;

// Sets the Mirror completion bit for its scope (with the unlocks setting) and puts it back after.
struct MirrorUnlockScope {
    uint32_t word = 0, saved = 0;
    MirrorUnlockScope() {
        if (!LocalVs::UnlocksEnabled()) return;
        const uint32_t sdm = Memory::Read32(kSaveManager);
        const int16_t license = sdm ? int16_t(Memory::Read16(sdm + 0x36)) : -1;
        if (license < 0) return;
        word = sdm + kLicenseBase + uint32_t(license) * kLicenseSize + kMirrorWord;
        saved = Memory::Read32(word);
        Memory::Write32(word, saved | kMirrorBit);
    }
    ~MirrorUnlockScope() { if (word) Memory::Write32(word, saved); }
};

extern "C" void VsSettings_CreateControl_808530b8(CpuContext* ctx) {
    if (ctx->gpr[4] != kVsPage.rowId) {
        MirrorUnlockScope unlock;
        MKW_GUEST_NEXT(808530B8)(ctx);
        return;
    }
    ctx->gpr[3] = BuildRow(ctx, ctx->gpr[3], kVsPage);
}

extern "C" void BattleSettings_CreateControl_8083bfe0(CpuContext* ctx) {
    if (ctx->gpr[4] != kBattlePage.rowId) {
        MKW_GUEST_NEXT(8083BFE0)(ctx);
        return;
    }
    ctx->gpr[3] = BuildRow(ctx, ctx->gpr[3], kBattlePage);
}
REGISTER_NATIVE_WRAPPER_AS(0x8083BFE0, BattleSettings_CreateControl_8083bfe0, "BattleSettings_CreateControl_8083bfe0");
REGISTER_NATIVE_WRAPPER_AS(0x808530B8, VsSettings_CreateControl_808530b8, "VsSettings_CreateControl_808530b8");

// The OK button, the page's only external control, moves below the new row.
extern "C" void VsSettings_CreateExternalControl_80853028(CpuContext* ctx) {
    MKW_GUEST_NEXT(80853028)(ctx);
    const uint32_t ok = ctx->gpr[3];
    if (ok == 0) return;
    ShiftY(ok, -kOkDrop);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t scaleY = ok + 4 + i * 0x18 + 0x10;
        Memory::WriteFloat32(scaleY, Memory::ReadFloat32(scaleY) * kOkScaleY);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80853028, VsSettings_CreateExternalControl_80853028, "VsSettings_CreateExternalControl_80853028");

// OnUpDownChange(page, control, hudSlot, option): the race count's handler stores the option in
// the license; the new row's option is the racer count minus one.
extern "C" void VsSettings_OnUpDownChange_80853998(CpuContext* ctx) {
    if (IsRow(ctx->gpr[3], ctx->gpr[4])) {
        LocalVs::SetDesiredCount(int(ctx->gpr[6]) + 1);
        return;
    }
    MKW_GUEST_NEXT(80853998)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80853998, VsSettings_OnUpDownChange_80853998, "VsSettings_OnUpDownChange_80853998");

// OnTextChange(page, textControl, option): the race count's looks the option up in its own table.
extern "C" void VsSettings_OnTextChange_80853a10(CpuContext* ctx) {
    if (IsRowText(ctx->gpr[3], ctx->gpr[4])) {
        SetRowValue(ctx, ctx->gpr[4], ctx->gpr[5]);
        return;
    }
    MKW_GUEST_NEXT(80853A10)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80853A10, VsSettings_OnTextChange_80853a10, "VsSettings_OnTextChange_80853a10");

// OnUpDownSelect(page, control, hudSlot) sets the help bar to message 3465; for the new row that
// one lookup answers "Set the number of racers.".
extern "C" void VsSettings_OnUpDownSelect_80853988(CpuContext* ctx) {
    gSettingHelp = IsRow(ctx->gpr[3], ctx->gpr[4]);
    gHelpBmg = kVsPage.helpBmg;
    MKW_GUEST_NEXT(80853988)(ctx);
    gSettingHelp = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x80853988, VsSettings_OnUpDownSelect_80853988, "VsSettings_OnUpDownSelect_80853988");

// Menu.bmg 3414 "Mirror" and 3418 "150cc class with mirrored courses." when Mirror is 200cc.
constexpr uint32_t kMirrorBmg = 3414, kMirrorHelpBmg = 3418;

// The CPU row (local_vs.cpp PrepareRace turns the choice into Hard CPUs with ai_impossible.cpp's
// tables for Very Hard and Impossible). Labels 3421-3424 Easy, Normal, Hard, Off; help 3425-3428.
//   2-4P (4 options): Normal, Hard, Very Hard, Impossible (each stock label one along).
//   1P (3 buttons; Off exists with 2+ local players only): Hard, Very Hard, Impossible.
// The section's local player count: SectionMgr (0x809C1E38) +0x98 -> +0x124, low byte.
constexpr uint32_t kCpuEasyBmg = 3421, kCpuHardBmg = 3423, kCpuOffBmg = 3424;
constexpr uint32_t kCpuEasyHelpBmg = 3425, kCpuHardHelpBmg = 3427, kCpuOffHelpBmg = 3428;
uint32_t SectionLocalPlayers() {
    const uint32_t mgr = Memory::Read32(0x809C1E38);
    const uint32_t section = mgr ? Memory::Read32(mgr + 0x98) : 0;
    return section ? Memory::Read8(section + 0x124 + 3) : 0;
}

extern "C" void BmgHolder_GetMsgId_805f8c88(CpuContext* ctx) {
    const uint32_t id = ctx->gpr[4];
    const bool help = id >= kCpuEasyHelpBmg && id <= kCpuOffHelpBmg;
    if ((id >= kCpuEasyBmg && id <= kCpuOffBmg) || help) {
        // shown: 0 Normal, 1 Hard, 2 Very Hard, 3 Impossible
        const uint32_t option = id - (help ? kCpuEasyHelpBmg : kCpuEasyBmg);
        const uint32_t shown = SectionLocalPlayers() <= 1 ? option + 1 : option;
        if (shown == 3) {
            ctx->gpr[4] = CustomMessages::LabelBmg(help ? 24 : 23, help ? "CPU players are much faster." : "Impossible", id);
        } else if (shown == 2) {
            ctx->gpr[4] = CustomMessages::LabelBmg(help ? 26 : 25, help ? "CPU players are faster." : "Very Hard", id);
        } else {
            ctx->gpr[4] = (help ? kCpuEasyHelpBmg : kCpuEasyBmg) + shown + 1;  // Normal or Hard
        }
    }
    if (Cc200::ReplacesMirror() && (ctx->gpr[4] == kMirrorBmg || ctx->gpr[4] == kMirrorHelpBmg)) {
        const bool help = ctx->gpr[4] == kMirrorHelpBmg;
        ctx->gpr[4] = CustomMessages::LabelBmg(help ? 22 : 21, help ? "200cc class with brake drifting." : "200cc",
                                               help ? kMirrorHelpBmg : kMirrorBmg);
    }
    if (uint32_t custom = ctx->gpr[4], fontBmg = 0; CustomMessages::IsCustom(custom, fontBmg)) {  // custom_messages.h
        ctx->gpr[4] = fontBmg;
        MKW_GUEST_NEXT(805F8C88)(ctx);
        // Not in this holder (-1): say so, and the game asks the next holder (Common.bmg) the same.
        if (ctx->gpr[3] != 0xFFFFFFFFu) ctx->gpr[3] = CustomMessages::Index(custom, ctx->gpr[3]);
        return;
    }
    if (gSettingHelp && ctx->gpr[4] == gHelpBmg) {
        MKW_GUEST_NEXT(805F8C88)(ctx);
        gHelpMsgIndex = ctx->gpr[3];
        ctx->gpr[3] = kHelpMsg;
        return;
    }
    if (gLoadingRow && ctx->gpr[4] == kLabelBmg) {
        MKW_GUEST_NEXT(805F8C88)(ctx);
        gLabelMsg = ctx->gpr[3];
        ctx->gpr[3] = kInjectedMsg;
        return;
    }
    MKW_GUEST_NEXT(805F8C88)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805F8C88, BmgHolder_GetMsgId_805f8c88, "BmgHolder_GetMsgId_805f8c88");

extern "C" void BmgHolder_GetMsgByMsgId_805f8cf0(CpuContext* ctx) {
    if (uint32_t text = 0; CustomMessages::Text(ctx->gpr[4], text)) { ctx->gpr[3] = text; return; }
    if (ctx->gpr[4] == kInjectedMsg || ctx->gpr[4] == kHelpMsg) {
        ctx->gpr[3] = ctx->gpr[4] == kHelpMsg ? kHelpText : kLabelText;
        return;
    }
    MKW_GUEST_NEXT(805F8CF0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805F8CF0, BmgHolder_GetMsgByMsgId_805f8cf0, "BmgHolder_GetMsgByMsgId_805f8cf0");

extern "C" void BmgHolder_GetFontIndex_805f8d2c(CpuContext* ctx) {
    if (uint32_t real = 0; CustomMessages::FontIndex(ctx->gpr[4], real)) ctx->gpr[4] = real;
    if (ctx->gpr[4] == kInjectedMsg) ctx->gpr[4] = gLabelMsg;
    else if (ctx->gpr[4] == kHelpMsg) ctx->gpr[4] = gHelpMsgIndex;
    MKW_GUEST_NEXT(805F8D2C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805F8D2C, BmgHolder_GetFontIndex_805f8d2c, "BmgHolder_GetFontIndex_805f8d2c");

// The battle rules page's handlers, as the VS page's above.
extern "C" void BattleSettings_CreateExternalControl_8083bf50(CpuContext* ctx) {
    MKW_GUEST_NEXT(8083BF50)(ctx);
    const uint32_t ok = ctx->gpr[3];
    if (ok == 0) return;
    ShiftY(ok, -kOkDrop);
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t scaleY = ok + 4 + i * 0x18 + 0x10;
        Memory::WriteFloat32(scaleY, Memory::ReadFloat32(scaleY) * kOkScaleY);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x8083BF50, BattleSettings_CreateExternalControl_8083bf50, "BattleSettings_CreateExternalControl_8083bf50");

extern "C" void BattleSettings_OnUpDownChange_8083c848(CpuContext* ctx) {
    if (IsRow(ctx->gpr[3], ctx->gpr[4])) {
        LocalVs::SetDesiredCount(int(ctx->gpr[6]) + 1);
        return;
    }
    MKW_GUEST_NEXT(8083C848)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8083C848, BattleSettings_OnUpDownChange_8083c848, "BattleSettings_OnUpDownChange_8083c848");

extern "C" void BattleSettings_OnTextChange_8083c8c0(CpuContext* ctx) {
    if (IsRowText(ctx->gpr[3], ctx->gpr[4])) {
        SetRowValue(ctx, ctx->gpr[4], ctx->gpr[5]);
        return;
    }
    MKW_GUEST_NEXT(8083C8C0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8083C8C0, BattleSettings_OnTextChange_8083c8c0, "BattleSettings_OnTextChange_8083c8c0");

extern "C" void BattleSettings_OnUpDownSelect_8083c838(CpuContext* ctx) {
    gSettingHelp = IsRow(ctx->gpr[3], ctx->gpr[4]);
    gHelpBmg = kBattlePage.helpBmg;
    MKW_GUEST_NEXT(8083C838)(ctx);
    gSettingHelp = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x8083C838, BattleSettings_OnUpDownSelect_8083c838, "BattleSettings_OnUpDownSelect_8083c838");

// The GP class page offers Mirror (200cc here) on the same completion bit, read inline in its
// OnInit (0x8083F4A8) and CreateExternalControl (0x8083F580).
extern "C" void GpClassSelect_OnInit_8083f4a8(CpuContext* ctx) {
    MirrorUnlockScope unlock;
    MKW_GUEST_NEXT(8083F4A8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8083F4A8, GpClassSelect_OnInit_8083f4a8, "GpClassSelect_OnInit_8083f4a8");

extern "C" void GpClassSelect_CreateExternalControl_8083f580(CpuContext* ctx) {
    MirrorUnlockScope unlock;
    MKW_GUEST_NEXT(8083F580)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8083F580, GpClassSelect_CreateExternalControl_8083f580, "GpClassSelect_CreateExternalControl_8083f580");
