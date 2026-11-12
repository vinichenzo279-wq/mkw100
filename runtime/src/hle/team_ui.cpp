// mkw100 team colours in the race HUD (teams.h). As Retro Rewind's extended teams do
// (rr-pulsar ExtendedTeamSelectMisc.cpp, GPL-3), existing layout panes are tinted at runtime; no
// assets are added:
//   name balloons: every racer's character name (chara_name) in its team colour, after
//       CtrlRaceNameBalloon::UpdateInfo;
//   minimap: the local player's teammates get their marker's two shadow panes shown in the team
//       colour and are drawn on top, after CtrlRace2DMapCharacter::OnUpdate;
//   results rows: team_color_c (select_base on the local player's row) shown in the team colour,
//       whenever local_vs.cpp fills a row (TintResultRow).
// Name balloons: the tint is applied after the game's update, which overwrites what the balloon's
// animations set. Minimap markers: their colour animation repaints the outline every frame, so
// while CtrlRace2DMapCharacter::InitSelf / CalculatePosition run, AnimationGroup::
// PlayAnimationAtFrameAndDisable only deactivates the group, as RR does at those call sites.

#include "teams.h"

#include "abi_bridge.h"
#include "custom_messages.h"
#include "local_vs.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

constexpr uint32_t kGetPaneByName = 0x805E8368;     // Layout::GetPaneByName(layout, name)
constexpr uint32_t kSetPaneVisibility = 0x8063E54C; // LayoutUIControl::SetPaneVisibility(control, name, bool)
constexpr uint32_t kLayout = 0xA8;                  // LayoutUIControl::layout
constexpr uint32_t kRootPane = 0x170;               // LayoutUIControl::rootPane
constexpr uint32_t kZIndex = 0x84;                  // UIControl::zIdx
constexpr uint32_t kPaneAlpha = 0xB8;               // nw4r::lyt::Pane::alpha
constexpr uint32_t kPaneTransZ = 0x34;              // Pane::trans.z
constexpr uint32_t kGetMaterial = 0x68;             // Pane vtable: GetMaterial()
constexpr uint32_t kTevColours = 0x10;              // Material: GX::ColorS10 tevColours[3]
constexpr uint32_t kTextColour = 0xDC;              // TextBox: ut::Color color1[2]

// Pane names, the game's own strings.
constexpr uint32_t kCharaName = 0x808A9F56;         // "chara_name"
constexpr uint32_t kShadow0 = 0x808A9504;           // "chara_shadow_0_0"
constexpr uint32_t kShadow1 = 0x808A9515;           // "chara_shadow_0_1"

// Pane names the game's own strings do not have, in the fixed runtime table span.
constexpr uint32_t kTeamColourPane = ArrayOverflow::kMenuText + 0x2000;  // "team_color_c"
constexpr uint32_t kSelectBasePane = kTeamColourPane + 0x20;             // "select_base"
constexpr uint32_t kPaneFlag = 0xBB;                                     // Pane::flag, bit 0 visible
constexpr uint32_t kRacedata = 0x809BD728;
constexpr uint32_t kRaceScenarioPlayers = 0x28;                          // Racedata + 0x20 + 8
constexpr uint32_t kPlayerType = 0x10;                                   // 0 = real local

constexpr uint32_t kBalloonPlayer = 4;              // UpdateInfo's player id argument (r4)
constexpr uint32_t kMapPlayer = 0x1B4;              // CtrlRace2DMapCharacter: u8 player id
constexpr uint32_t kMapChara = 0x1B8;               // charaPane, charaShadow0Pane, charaShadow1Pane

uint32_t TeamUiCall(CpuContext* ctx, const CpuContext& saved, uint32_t fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    *ctx = saved;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

bool TeamUiInRam(uint32_t address) { return address >= 0x80000000u && address < 0x98000000u; }

void TeamUiTint(CpuContext* ctx, const CpuContext& saved, uint32_t pane, const Teams::Colour& c, uint32_t first,
                  uint32_t last, int alpha) {
    const uint32_t material = TeamUiCall(ctx, saved, Memory::Read32(Memory::Read32(pane) + kGetMaterial), pane);
    if (!TeamUiInRam(material)) return;
    for (uint32_t i = first; i <= last; ++i) {
        const uint32_t colour = material + kTevColours + i * 8;  // s16 r, g, b, a
        Memory::Write16(colour + 0, c.r);
        Memory::Write16(colour + 2, c.g);
        Memory::Write16(colour + 4, c.b);
        if (alpha >= 0) Memory::Write16(colour + 6, uint16_t(alpha));
    }
}

void WriteName(uint32_t address, const char* text) {
    for (uint32_t i = 0;; ++i) {
        Memory::Write8(address + i, uint8_t(text[i]));
        if (!text[i]) break;
    }
}

} // namespace

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

void Teams::TintResultRow(CpuContext* ctx, uint32_t control, uint32_t player) {
    if (!Teams::Active()) return;
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams) return;
    static bool named = false;
    if (!named) {
        named = true;
        WriteName(kTeamColourPane, "team_color_c");
        WriteName(kSelectBasePane, "select_base");
    }
    const CpuContext saved = *ctx;
    const uint32_t rd = Memory::Read32(kRacedata);
    const bool local = Memory::Read32(rd + MKW100_PlayerSlotOffset(player) + kRaceScenarioPlayers + kPlayerType) == 0;
    const uint32_t pane = TeamUiCall(ctx, saved, kGetPaneByName, control + kLayout, local ? kSelectBasePane : kTeamColourPane);
    if (TeamUiInRam(pane)) {
        Memory::Write8(pane + kPaneAlpha, local ? 255 : 200);
        Memory::Write8(pane + kPaneFlag, Memory::Read8(pane + kPaneFlag) | 1);
        TeamUiTint(ctx, saved, pane, Teams::ColourOf(team), 0, 1, 255);
    }
    *ctx = saved;
}

// CtrlRaceNameBalloon::UpdateInfo(this, playerId)
extern "C" void TeamUi_BalloonUpdateInfo_807f0044(CpuContext* ctx) {
    const uint32_t balloon = ctx->gpr[3];
    const uint32_t player = ctx->gpr[kBalloonPlayer] & 0xFF;
    MKW_GUEST_NEXT(807F0044)(ctx);
    if (!Teams::Active()) return;
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams) return;
    const Teams::Colour& c = Teams::ColourOf(team);
    const CpuContext saved = *ctx;
    const uint32_t name = TeamUiCall(ctx, saved, kGetPaneByName, balloon + kLayout, kCharaName);
    if (TeamUiInRam(name)) {
        Memory::Write8(name + kTextColour + 0, c.r);  // color1[0], top
        Memory::Write8(name + kTextColour + 1, c.g);
        Memory::Write8(name + kTextColour + 2, c.b);
        Memory::Write8(name + kTextColour + 3, 255);
        Memory::Write8(name + kTextColour + 4, c.r);  // color1[1], bottom
        Memory::Write8(name + kTextColour + 5, c.g);
        Memory::Write8(name + kTextColour + 6, c.b);
        Memory::Write8(name + kTextColour + 7, 255);
        // The text material's TEV colour 1 to white, as RR does, so the vertex colour shows.
        const uint32_t material = TeamUiCall(ctx, saved, Memory::Read32(Memory::Read32(name) + kGetMaterial), name);
        if (TeamUiInRam(material)) {
            for (uint32_t k = 0; k < 3; ++k) Memory::Write16(material + kTevColours + 8 + k * 2, 255);
        }
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807F0044, TeamUi_BalloonUpdateInfo_807f0044, "TeamUi_BalloonUpdateInfo_807f0044");

namespace {
bool gInMapMarker = false;  // game thread only
}

// AnimationGroup::PlayAnimationAtFrameAndDisable(group, id, frame)
extern "C" void TeamUi_PlayAnimationAndDisable_8063cb20(CpuContext* ctx) {
    if (gInMapMarker) {
        Memory::Write8(ctx->gpr[3] + 0x35, 0);  // isActive = false
        return;
    }
    MKW_GUEST_NEXT(8063CB20)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8063CB20, TeamUi_PlayAnimationAndDisable_8063cb20, "TeamUi_PlayAnimationAndDisable_8063cb20");

// CtrlRace2DMapCharacter::InitSelf(this) and ::CalculatePosition(this, pos, dest, r6)
extern "C" void TeamUi_MapCharacterInit_807eaecc(CpuContext* ctx) {
    gInMapMarker = Teams::Active();
    MKW_GUEST_NEXT(807EAECC)(ctx);
    gInMapMarker = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EAECC, TeamUi_MapCharacterInit_807eaecc, "TeamUi_MapCharacterInit_807eaecc");
extern "C" void TeamUi_MapCharacterPosition_807eb3cc(CpuContext* ctx) {
    gInMapMarker = Teams::Active();
    MKW_GUEST_NEXT(807EB3CC)(ctx);
    gInMapMarker = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EB3CC, TeamUi_MapCharacterPosition_807eb3cc, "TeamUi_MapCharacterPosition_807eb3cc");

// CtrlRace2DMapCharacter::OnUpdate(this)
extern "C" void TeamUi_MapCharacterUpdate_807eb250(CpuContext* ctx) {
    const uint32_t marker = ctx->gpr[3];
    MKW_GUEST_NEXT(807EB250)(ctx);
    if (!Teams::Active()) return;
    const uint32_t player = Memory::Read8(marker + kMapPlayer);
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams || team != Teams::TeamOf(0)) return;  // the local player's team only
    const Teams::Colour& c = Teams::ColourOf(team);
    const CpuContext saved = *ctx;
    TeamUiCall(ctx, saved, kSetPaneVisibility, marker, kShadow0, 1);
    TeamUiCall(ctx, saved, kSetPaneVisibility, marker, kShadow1, 1);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t pane = Memory::Read32(marker + kMapChara + 4 * i);
        if (!TeamUiInRam(pane)) continue;
        Memory::Write8(pane + kPaneAlpha, 255);
        if (i > 0) TeamUiTint(ctx, saved, pane, c, 0, 1, 255);
    }
    Memory::Write32(marker + kZIndex, 20);  // over the other markers
    const uint32_t root = Memory::Read32(marker + kRootPane);
    if (TeamUiInRam(root)) Memory::WriteFloat32(root + kPaneTransZ, 20.0f);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EB250, TeamUi_MapCharacterUpdate_807eb250, "TeamUi_MapCharacterUpdate_807eb250");

// AnimationGroup::PlayAnimationAtFrame(group, id, frame) from the results rows (CtrlRaceResult):
// their colour animation repaints the row every frame over the team tint, so with teams it is
// switched off where the row is loaded and skipped where the row is filled, as Retro Rewind's
// extended teams do (ExtendedTeamSelectMisc.cpp, CtrlRaceResult_InitPatchAnimation /
// CalcSkipAnimation, same call sites).
extern "C" void TeamUi_PlayAnimation_8063c91c(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    const bool load = lr == 0x807F4F78 || lr == 0x807F4F90;               // CtrlRaceResult::Load
    const bool fill = lr == 0x807F631C || lr == 0x807F63A8 || lr == 0x807F6414;  // CtrlRaceResult::Fill
    if ((load || fill) && Teams::Active()) {
        Memory::Write8(ctx->gpr[3] + 0x35, 0);  // isActive = false (Load may run before teams are dealt)
        return;
    }
    MKW_GUEST_NEXT(8063C91C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8063C91C, TeamUi_PlayAnimation_8063c91c, "TeamUi_PlayAnimation_8063c91c");

// The pre-race team screen (Pages::VSTeamsView): two CtrlMenuTeamConfirmTeam columns of six rows
// (TeamConfirmBlueRed: a red or blue background, character icon, name), built for six a team.
// In a Team VS series of more than 12 racers it becomes the team settings screen:
//   left column, the settings (Teams::), up/down picks one and left/right changes it;
//   right column, the teams as dealt now, re-dealt on every change ("Red Team (13)" in its colour,
//   "You" on the player's; with more than six teams two share a row).
// Backgrounds and icons are hidden. The teams dealt here are the series' (Teams::DealForMenu).
// Custom per-colour sizes: "Size of" picks a colour and the row below sets its racers.
namespace {

constexpr uint32_t kConfirmTeam = 0x174;     // CtrlMenuTeamConfirmTeam: the column's team (0, 1)
constexpr uint32_t kConfirmRows = 0x178;     // six row controls
constexpr uint32_t kConfirmRowStride = 0x174;
constexpr uint32_t kConfirmRowCount = 6;
constexpr uint32_t kConfirmRowHidden = 0x80; // row: isHidden (InitSelf: 1, then 0 per filled row)
constexpr uint32_t kConfirmVtable = 0x808D37D8;
constexpr uint32_t kSetMessage = 0x8063DCBC; // LayoutUIControl::SetMessage(control, pane, bmg, args)
constexpr uint32_t kGetControllerHolder = 0x805EEE0C;  // ManipulatorManager::GetControllerHolder(0)
constexpr uint32_t kMenuFlags = 0xC10 + 0xB70;    // menu scenario settings modeFlags (bit 2 teams)
constexpr uint32_t kMenuGameMode = 0xC10 + 0xB50; // menu scenario settings gameMode (1 VS)
constexpr uint32_t kScreenNames = ArrayOverflow::kMenuText + 0x2040;  // pane name strings
constexpr uint32_t kMiiNamePane = kScreenNames;           // "mii_name"
constexpr uint32_t kIconPane = kScreenNames + 0x10;       // "chara_icon"
constexpr uint32_t kIconShadowPane = kScreenNames + 0x20; // "chara_icon_sha"
constexpr uint32_t kRedPane = kScreenNames + 0x30;        // "red_null"
constexpr uint32_t kBluePane = kScreenNames + 0x40;       // "blue_null"
constexpr uint32_t kSettingSlot = 8;                      // CustomMessages::LabelBmg slots 8..13
constexpr uint32_t kPairSlot = 14;                        // ..14, 15: rows holding two teams
constexpr Teams::Colour kWhite{255, 255, 255, "White"};
constexpr Teams::Colour kPicked{255, 225, 60, "Picked"};

enum TeamSetting : uint32_t { kSettingBy, kSettingAmount, kSettingScore, kSettingHits, kSettingColour, kSettingCustomSize,
                              kSettingRows };

uint32_t gScreenColumn[2] = {0, 0};  // left, right; game thread only
uint32_t gScreenPick = 0;
uint32_t gScreenRacers = 0;
uint32_t gScreenCustomColour = 0;  // the colour "Size of" edits
uint32_t gScreenHeld = 0;          // frames left/right has been held (repeat)

// Which settings the screen shows: Team VS (2-8 teams) or battle (red/blue sizes).
enum class TeamScreen { kNone, kVs, kBattle };
TeamScreen gScreenKind = TeamScreen::kNone;

// Rows in use: the custom size row only with custom sizes.
// Battle rows: split (even / custom), red's size (custom only), your team.
enum BattleSetting : uint32_t { kBattleSplit, kBattleRed, kBattleTeam };
uint32_t TeamUiSettingCount() {
    if (gScreenKind == TeamScreen::kBattle) return 3;
    return Teams::CustomSizes() ? kSettingRows : kSettingCustomSize;
}

std::string TeamUiBattleText(uint32_t setting) {
    const uint32_t racers = uint32_t(LocalVs::DesiredCount());
    switch (setting) {
    case kBattleSplit: return std::string("Teams: ") + (Teams::BattleRedSize() ? "Custom sizes" : "Even");
    case kBattleRed:
        return Teams::BattleRedSize() ? "Red racers: " + std::to_string(Teams::BattleRedFor(racers)) : "Red racers: half";
    default: {
        static const char* const kChoice[3] = {"Random", "Red", "Blue"};
        return std::string("Your team: ") + kChoice[Teams::BattleTeamChoice() % 3];
    }
    }
}

void TeamUiBattleChange(uint32_t setting, int step) {
    const uint32_t racers = uint32_t(LocalVs::DesiredCount());
    switch (setting) {
    case kBattleSplit:
        Teams::SetBattleRedSize(Teams::BattleRedSize() ? 0 : (racers + 1) / 2);
        break;
    case kBattleRed:
        if (Teams::BattleRedSize() == 0) Teams::SetBattleRedSize((racers + 1) / 2);
        Teams::SetBattleRedSize(uint32_t(std::clamp(int(Teams::BattleRedFor(racers)) + step, 1, int(racers) - 1)));
        break;
    default:
        Teams::SetBattleTeamChoice((Teams::BattleTeamChoice() + 3 + uint32_t(step)) % 3);
        break;
    }
}

TeamScreen TeamUiScreenKind() {
    const uint32_t rd = Memory::Read32(kRacedata);
    const int racers = LocalVs::DesiredCount();
    if (rd == 0 || racers <= 12 || (Memory::Read32(rd + kMenuFlags) & 2) == 0) return TeamScreen::kNone;
    const uint32_t mode = Memory::Read32(rd + kMenuGameMode);
    return mode == 1 ? TeamScreen::kVs : mode == 3 ? TeamScreen::kBattle : TeamScreen::kNone;
}
bool TeamUiScreenActive() { return TeamUiScreenKind() != TeamScreen::kNone; }

void TeamUiTextColour(CpuContext* ctx, const CpuContext& saved, uint32_t text, const Teams::Colour& c) {
    for (uint32_t k = 0; k < 2; ++k) {
        Memory::Write8(text + kTextColour + 4 * k + 0, c.r);
        Memory::Write8(text + kTextColour + 4 * k + 1, c.g);
        Memory::Write8(text + kTextColour + 4 * k + 2, c.b);
        Memory::Write8(text + kTextColour + 4 * k + 3, 255);
    }
    const uint32_t material = TeamUiCall(ctx, saved, Memory::Read32(Memory::Read32(text) + kGetMaterial), text);
    if (TeamUiInRam(material)) {
        for (uint32_t k = 0; k < 3; ++k) Memory::Write16(material + kTevColours + 8 + k * 2, 255);
    }
}

uint32_t TeamUiRow(uint32_t column, uint32_t k) { return column + kConfirmRows + k * kConfirmRowStride; }

void TeamUiSetRow(CpuContext* ctx, const CpuContext& saved, uint32_t row, uint32_t bmg, const Teams::Colour& colour) {
    Memory::Write8(row + kConfirmRowHidden, 0);
    for (uint32_t pane : {kIconPane, kIconShadowPane, kRedPane, kBluePane}) TeamUiCall(ctx, saved, kSetPaneVisibility, row, pane, 0);
    *ctx = saved;
    ctx->gpr[3] = row;
    ctx->gpr[4] = kMiiNamePane;
    ctx->gpr[5] = bmg;
    ctx->gpr[6] = 0;  // no message arguments
    InvokeIndirectCpu(kSetMessage, ctx);
    const uint32_t text = TeamUiCall(ctx, saved, kGetPaneByName, row + kLayout, kMiiNamePane);
    if (TeamUiInRam(text)) TeamUiTextColour(ctx, saved, text, colour);
}

std::string TeamUiSettingText(uint32_t setting) {
    switch (setting) {
    case kSettingBy:
        return std::string("Teams by: ") + (Teams::CustomSizes() ? "Custom" : Teams::TeamSize() ? "Size" : "Count");
    case kSettingAmount:
        if (Teams::CustomSizes()) return std::string("Size of: ") + Teams::ColourByIndex(gScreenCustomColour).name;
        if (Teams::TeamSize()) return "Per team: " + std::to_string(Teams::TeamSize());
        return "Teams: " + std::to_string(Teams::Count());
    case kSettingScore:
        return std::string("Score: ") + (Teams::ScoreByAverage() ? "Average" : "Total");
    case kSettingHits:
        return std::string("Team hits: ") + (Teams::TeamHits() ? "On" : "Off");
    case kSettingCustomSize:
        return std::string(Teams::ColourByIndex(gScreenCustomColour).name) + " racers: " +
               std::to_string(Teams::ColourSize(gScreenCustomColour));
    default: {
        const uint32_t mine = Teams::PlayerColour();
        return std::string("Your colour: ") + (mine < Teams::kMaxTeams ? Teams::ColourByIndex(mine).name : "Random");
    }
    }
}

void TeamUiChange(uint32_t setting, int step) {
    switch (setting) {
    case kSettingBy: {
        const int by = Teams::CustomSizes() ? 2 : Teams::TeamSize() ? 1 : 0;
        const int next = (by + step + 3) % 3;
        Teams::SetCustomSizes(next == 2);
        if (next == 0) Teams::SetTeamSize(0);
        if (next == 1) Teams::SetTeamSize(std::max<uint32_t>(Teams::TeamSize(), 4));
        break;
    }
    case kSettingAmount:
        if (Teams::CustomSizes()) {
            gScreenCustomColour = (gScreenCustomColour + Teams::kMaxTeams + uint32_t(step)) % Teams::kMaxTeams;
            break;
        }
        if (Teams::TeamSize()) Teams::SetTeamSize(uint32_t(std::clamp(int(Teams::TeamSize()) + step, 2, 64)));
        else Teams::SetCount(uint32_t(std::clamp(int(Teams::Count()) + step, 2, int(Teams::kMaxTeams))));
        break;
    case kSettingScore: Teams::SetScoreByAverage(!Teams::ScoreByAverage()); break;
    case kSettingHits: Teams::SetTeamHits(!Teams::TeamHits()); break;
    case kSettingCustomSize:
        Teams::SetColourSize(gScreenCustomColour,
                             uint32_t(std::clamp(int(Teams::ColourSize(gScreenCustomColour)) + step, 0, int(PlayerSlots::kMaxPlayers))));
        break;
    default: {
        // Random, then the 8 colours.
        const int at = Teams::PlayerColour() < Teams::kMaxTeams ? int(Teams::PlayerColour()) + 1 : 0;
        const int next = (at + step + int(Teams::kMaxTeams) + 1) % int(Teams::kMaxTeams + 1);
        Teams::SetPlayerColour(next == 0 ? Teams::kNoTeam : uint32_t(next - 1));
        break;
    }
    }
}

void TeamUiRenderLeft(CpuContext* ctx, const CpuContext& saved, uint32_t column) {
    for (uint32_t k = 0; k < kConfirmRowCount; ++k) {
        const uint32_t row = TeamUiRow(column, k);
        if (k >= TeamUiSettingCount()) { Memory::Write8(row + kConfirmRowHidden, 1); continue; }
        std::string text = gScreenKind == TeamScreen::kBattle ? TeamUiBattleText(k) : TeamUiSettingText(k);
        if (k == gScreenPick) text = "< " + text + " >";
        // The custom size row in its colour, the rest white; the picked one yellow.
        const Teams::Colour& plain = gScreenKind == TeamScreen::kVs && k == kSettingCustomSize
                                         ? Teams::ColourByIndex(gScreenCustomColour) : kWhite;
        TeamUiSetRow(ctx, saved, row, CustomMessages::LabelBmg(kSettingSlot + k, text), k == gScreenPick ? kPicked : plain);
    }
}

void TeamUiRenderBattleRight(CpuContext* ctx, const CpuContext& saved, uint32_t column) {
    const uint32_t racers = uint32_t(LocalVs::DesiredCount());
    const uint32_t red = Teams::BattleRedFor(racers);
    const uint32_t choice = Teams::BattleTeamChoice();
    for (uint32_t k = 0; k < kConfirmRowCount; ++k) {
        const uint32_t row = TeamUiRow(column, k);
        if (k >= 2) { Memory::Write8(row + kConfirmRowHidden, 1); continue; }
        const uint32_t colour = k == 0 ? 0 : 4;  // Red, Blue in the colour table
        std::string label = std::string(k == 0 ? "Red" : "Blue") + " Team (" + std::to_string(k == 0 ? red : racers - red) + ")";
        if (choice == k + 1) label += " You";
        TeamUiSetRow(ctx, saved, row, CustomMessages::TeamLabelBmg(colour, label), Teams::ColourByIndex(colour));
    }
}

void TeamUiRenderRight(CpuContext* ctx, const CpuContext& saved, uint32_t column) {
    if (gScreenKind == TeamScreen::kBattle) {
        TeamUiRenderBattleRight(ctx, saved, column);
        return;
    }
    const uint32_t teams = Teams::InPlay();
    const uint32_t mine = Teams::TeamOf(0);
    const bool paired = teams > kConfirmRowCount;
    for (uint32_t k = 0; k < kConfirmRowCount; ++k) {
        const uint32_t row = TeamUiRow(column, k);
        const uint32_t a = paired ? 2 * k : k;
        if (a >= teams) { Memory::Write8(row + kConfirmRowHidden, 1); continue; }
        if (!paired) {
            const Teams::Colour& colour = Teams::ColourOf(a);
            std::string label = std::string(colour.name) + " Team (" + std::to_string(Teams::Members(a)) + ")";
            if (a == mine) label += " You";
            TeamUiSetRow(ctx, saved, row, CustomMessages::TeamLabelBmg(Teams::ColourIndexOf(a), label), colour);
            continue;
        }
        // Two teams a row, white; the player's team marked with "*".
        std::string label;
        for (uint32_t b = a; b < a + 2 && b < teams; ++b) {
            if (b > a) label += "  ";
            label += std::string(Teams::ColourOf(b).name) + " " + std::to_string(Teams::Members(b)) + (b == mine ? "*" : "");
        }
        const uint32_t slot = k < 2 ? kPairSlot + k : Teams::ColourIndexOf(a);  // 4 rows at most: 2 free slots + colour slots
        const uint32_t bmg = k < 2 ? CustomMessages::LabelBmg(slot, label) : CustomMessages::TeamLabelBmg(slot, label);
        TeamUiSetRow(ctx, saved, row, bmg, kWhite);
    }
}

void TeamUiRenderColumn(CpuContext* ctx, const CpuContext& saved, uint32_t column) {
    if (column == 0) return;
    if (Memory::Read32(column + kConfirmTeam) == 0) TeamUiRenderLeft(ctx, saved, column);
    else TeamUiRenderRight(ctx, saved, column);
}

} // namespace

// CtrlMenuTeamConfirmTeam::InitSelf(this)
extern "C" void TeamUi_ConfirmTeamInit_807e9f5c(CpuContext* ctx) {
    const uint32_t column = ctx->gpr[3];
    MKW_GUEST_NEXT(807E9F5C)(ctx);
    if (!TeamUiScreenActive()) return;
    static bool named = false;
    if (!named) {
        named = true;
        WriteName(kMiiNamePane, "mii_name");
        WriteName(kIconPane, "chara_icon");
        WriteName(kIconShadowPane, "chara_icon_sha");
        WriteName(kRedPane, "red_null");
        WriteName(kBluePane, "blue_null");
    }
    gScreenKind = TeamUiScreenKind();
    gScreenRacers = uint32_t(LocalVs::DesiredCount());
    if (gScreenKind == TeamScreen::kVs) Teams::DealForMenu(gScreenRacers);
    const uint32_t side = Memory::Read32(column + kConfirmTeam) == 0 ? 0 : 1;
    gScreenColumn[side] = column;
    if (side == 0) gScreenPick = 0;
    const CpuContext saved = *ctx;
    TeamUiRenderColumn(ctx, saved, column);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807E9F5C, TeamUi_ConfirmTeamInit_807e9f5c, "TeamUi_ConfirmTeamInit_807e9f5c");

// LayoutUIControl::Update (0x8063DB00, every control every frame): on the left settings column,
// up/down picks a setting and left/right changes it; both columns are redrawn after a change.
extern "C" void TeamUi_ControlUpdate_8063db00(CpuContext* ctx) {
    const uint32_t control = ctx->gpr[3];
    MKW_GUEST_NEXT(8063DB00)(ctx);
    if (control == 0 || control != gScreenColumn[0] || Memory::Read32(control) != kConfirmVtable) return;
    if (!TeamUiScreenActive()) return;
    const CpuContext saved = *ctx;
    const uint32_t holder = TeamUiCall(ctx, saved, kGetControllerHolder, 0);
    *ctx = saved;
    if (!TeamUiInRam(holder)) return;
    const uint32_t now = Memory::Read16(holder + 0x5C), before = Memory::Read16(holder + 0x90);  // UIState actions
    const auto pressed = [&](uint32_t bit) { return (now & bit) != 0 && (before & bit) == 0; };
    bool redraw = false, redeal = false;
    const uint32_t rows = TeamUiSettingCount();
    if (gScreenPick >= rows) gScreenPick = 0;
    // Left/right: once per press; held, the custom size repeats (after ~0.3 s, every 3 frames).
    const bool held = (now & 0x60) != 0;
    gScreenHeld = held ? gScreenHeld + 1 : 0;
    const bool sizeRow = gScreenKind == TeamScreen::kBattle ? gScreenPick == kBattleRed : gScreenPick == kSettingCustomSize;
    const bool repeat = sizeRow && gScreenHeld > 18 && gScreenHeld % 3 == 0;
    if (pressed(0x8)) { gScreenPick = (gScreenPick + rows - 1) % rows; redraw = true; }
    else if (pressed(0x10)) { gScreenPick = (gScreenPick + 1) % rows; redraw = true; }
    else if (pressed(0x20) || pressed(0x40) || repeat) {
        const int step = (now & 0x40) != 0 ? 1 : -1;
        redraw = true;
        if (gScreenKind == TeamScreen::kBattle) {
            TeamUiBattleChange(gScreenPick, step);  // counts only: battle teams are dealt at race start
        } else {
            TeamUiChange(gScreenPick, step);
            redeal = gScreenPick != kSettingScore && gScreenPick != kSettingHits &&
                     !(gScreenPick == kSettingAmount && Teams::CustomSizes());
        }
        if (gScreenPick >= TeamUiSettingCount()) gScreenPick = 0;
    }
    if (!redraw) return;
    if (redeal) Teams::DealForMenu(gScreenRacers, true);
    TeamUiRenderColumn(ctx, saved, gScreenColumn[0]);
    TeamUiRenderColumn(ctx, saved, gScreenColumn[1]);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8063DB00, TeamUi_ControlUpdate_8063db00, "TeamUi_ControlUpdate_8063db00");
