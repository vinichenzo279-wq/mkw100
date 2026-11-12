// mkw100: runtime messages for text the game's BMG files do not have. See custom_messages.h.
//
// Places: Race.bmg has "1st".."12th" (1301..1312) and nothing past it; results rows of 13th and
// beyond show message PlaceBmg(place) instead, "13th", "21st", ..., with the INF entry (font) of
// 1312. Ids 0x00FF0000 + place reach no BMG file; msg indices 0x7E00 + place stay clear of
// vs_racers_option.cpp's 0x7FFE/0x7FFF.

#include "custom_messages.h"

#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "memory.h"

#include <array>
#include <algorithm>
#include <string>

namespace {

constexpr uint32_t kPlaceBmgBase = 0x00FF0000;
constexpr uint32_t kTeamBmgBase = 0x00FE0000;   // + colour index
constexpr uint32_t kTeamIndexBase = 0x7D00;     // msg indices 0x7D00..0x7D0F
constexpr uint32_t kTeamFontBmg = 9501;         // Common.bmg: the player name
constexpr uint32_t kTeams = 8;
// Per colour, the plain name (total scoring) then the "avg" one (average scoring); at most 15
// characters (kTeamTextBytes).
constexpr uint32_t kTeamMessages = 2 * kTeams;
constexpr const char* kTeamNames[kTeamMessages] = {
    "Red Team", "Orange Team", "Yellow Team", "Green Team", "Blue Team", "Purple Team", "Pink Team", "Cyan Team",
    "Red Team avg", "Orange Team avg", "Yellow Team avg", "Green Team avg",
    "Blue Team avg", "Purple Team avg", "Pink Team avg", "Cyan Team avg"};
// Runtime labels per colour (the pre-race team screen): text rewritten before each use.
constexpr uint32_t kTeamLabelBmgBase = kTeamBmgBase + 0x100;
constexpr uint32_t kTeamLabelIndexBase = 0x7D40;
constexpr uint32_t kTeamLabelBytes = 48;            // 23 characters
constexpr uint32_t kPlaceIndexBase = 0x7E00;
constexpr uint32_t kFirstPlace = 13;
constexpr uint32_t kLastPlace = PlayerSlots::kMaxPlayers;
constexpr uint32_t kPlaceFontBmg = 1312;      // Race.bmg "12th"
constexpr uint32_t kTextBytes = 16;           // per place, UTF-16BE with terminator
// After vs_racers_option.cpp's label and help strings at kMenuText.
constexpr uint32_t kPlaceText = ArrayOverflow::kMenuText + 0x1000;
constexpr uint32_t kTeamText = ArrayOverflow::kMenuText + 0x1800;  // 16 x 32 bytes
constexpr uint32_t kTeamTextBytes = 32;
static_assert(kPlaceText + (PlayerSlots::kMaxPlayers - 13 + 1) * 16 <= kTeamText);  // 13th..128th
static_assert(kPlaceIndexBase + kLastPlace < 0x7FFE);
static_assert(kTeamText + kTeamMessages * kTeamTextBytes <= ArrayOverflow::kMenuText + 0x1A00);
constexpr uint32_t kTeamLabelText = ArrayOverflow::kMenuText + 0x1A00;
constexpr uint32_t kTeamLabelSlots = 19;  // 16: the racers row value; 17, 18: "200cc" for Mirror
static_assert(kTeamLabelText + kTeamLabelSlots * kTeamLabelBytes <= ArrayOverflow::kMenuText + 0x2000);  // team_ui strings follow

std::array<uint32_t, kLastPlace + 1> g_fontIndex{};  // msg index of 1312 in the holder that asked
std::array<uint32_t, kTeamMessages> g_teamFontIndex{};
std::array<uint32_t, kTeamLabelSlots> g_teamLabelFontIndex{};
std::array<uint32_t, kTeamLabelSlots> g_teamLabelFont{};  // font BMG per slot (0: kTeamFontBmg)
bool g_written = false;

void WriteTexts() {
    if (g_written) return;
    g_written = true;
    for (uint32_t place = kFirstPlace; place <= kLastPlace; ++place) {
        const uint32_t tens = place % 100, ones = place % 10;
        const char* suffix = (tens >= 11 && tens <= 13) ? "th" : ones == 1 ? "st" : ones == 2 ? "nd" : ones == 3 ? "rd" : "th";
        const std::string text = std::to_string(place) + suffix;
        const uint32_t base = kPlaceText + (place - kFirstPlace) * kTextBytes;
        for (uint32_t i = 0; i <= text.size(); ++i) Memory::Write16(base + 2 * i, i < text.size() ? uint8_t(text[i]) : 0);
    }
    for (uint32_t team = 0; team < kTeamMessages; ++team) {
        const std::string text = kTeamNames[team];
        const uint32_t base = kTeamText + team * kTeamTextBytes;
        for (uint32_t i = 0; i <= text.size(); ++i) Memory::Write16(base + 2 * i, i < text.size() ? uint8_t(text[i]) : 0);
    }
}

} // namespace

namespace CustomMessages {

uint32_t PlaceBmg(uint32_t place) {
    return place >= kFirstPlace && place <= kLastPlace ? kPlaceBmgBase + place : 0;
}

uint32_t LabelBmg(uint32_t slot, const std::string& text, uint32_t fontBmg) {
    if (slot >= kTeamLabelSlots) return 0;
    g_teamLabelFont[slot] = fontBmg;
    const uint32_t base = kTeamLabelText + slot * kTeamLabelBytes;
    const uint32_t length = std::min<uint32_t>(uint32_t(text.size()), kTeamLabelBytes / 2 - 1);
    for (uint32_t i = 0; i <= length; ++i) Memory::Write16(base + 2 * i, i < length ? uint8_t(text[i]) : 0);
    return kTeamLabelBmgBase + slot;
}

uint32_t TeamLabelBmg(uint32_t colour, const std::string& text) { return colour < kTeams ? LabelBmg(colour, text) : 0; }

uint32_t TeamNameBmg(uint32_t colour, bool average) {
    return colour < kTeams ? kTeamBmgBase + colour + (average ? kTeams : 0) : 0;
}

bool IsCustom(uint32_t bmgId, uint32_t& fontBmg) {
    if (bmgId >= kTeamLabelBmgBase && bmgId < kTeamLabelBmgBase + kTeamLabelSlots) {
        const uint32_t font = g_teamLabelFont[bmgId - kTeamLabelBmgBase];
        fontBmg = font ? font : kTeamFontBmg;
        return true;
    }
    if (bmgId >= kTeamBmgBase && bmgId < kTeamBmgBase + kTeamMessages) {
        fontBmg = kTeamFontBmg;
        return true;
    }
    if (bmgId < kPlaceBmgBase + kFirstPlace || bmgId > kPlaceBmgBase + kLastPlace) return false;
    fontBmg = kPlaceFontBmg;
    return true;
}

uint32_t Index(uint32_t bmgId, uint32_t fontMsgIndex) {
    WriteTexts();
    if (bmgId >= kTeamLabelBmgBase && bmgId < kTeamLabelBmgBase + kTeamLabelSlots) {
        g_teamLabelFontIndex[bmgId - kTeamLabelBmgBase] = fontMsgIndex;
        return kTeamLabelIndexBase + (bmgId - kTeamLabelBmgBase);
    }
    if (bmgId >= kTeamBmgBase && bmgId < kTeamBmgBase + kTeamMessages) {
        g_teamFontIndex[bmgId - kTeamBmgBase] = fontMsgIndex;
        return kTeamIndexBase + (bmgId - kTeamBmgBase);
    }
    const uint32_t place = bmgId - kPlaceBmgBase;
    g_fontIndex[place] = fontMsgIndex;
    return kPlaceIndexBase + place;
}

bool Text(uint32_t msgIndex, uint32_t& text) {
    if (msgIndex >= kTeamLabelIndexBase && msgIndex < kTeamLabelIndexBase + kTeamLabelSlots) {
        text = kTeamLabelText + (msgIndex - kTeamLabelIndexBase) * kTeamLabelBytes;
        return true;
    }
    if (msgIndex >= kTeamIndexBase && msgIndex < kTeamIndexBase + kTeamMessages) {
        text = kTeamText + (msgIndex - kTeamIndexBase) * kTeamTextBytes;
        return true;
    }
    if (msgIndex < kPlaceIndexBase + kFirstPlace || msgIndex > kPlaceIndexBase + kLastPlace) return false;
    text = kPlaceText + (msgIndex - kPlaceIndexBase - kFirstPlace) * kTextBytes;
    return true;
}

bool FontIndex(uint32_t msgIndex, uint32_t& realMsgIndex) {
    if (msgIndex >= kTeamLabelIndexBase && msgIndex < kTeamLabelIndexBase + kTeamLabelSlots) {
        realMsgIndex = g_teamLabelFontIndex[msgIndex - kTeamLabelIndexBase];
        return true;
    }
    if (msgIndex >= kTeamIndexBase && msgIndex < kTeamIndexBase + kTeamMessages) {
        realMsgIndex = g_teamFontIndex[msgIndex - kTeamIndexBase];
        return true;
    }
    if (msgIndex < kPlaceIndexBase + kFirstPlace || msgIndex > kPlaceIndexBase + kLastPlace) return false;
    realMsgIndex = g_fontIndex[msgIndex - kPlaceIndexBase];
    return true;
}

} // namespace CustomMessages
