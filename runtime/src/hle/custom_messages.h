#pragma once

#include <cstdint>
#include <string>

// mkw100: messages no BMG file has, answered at the game's three BMG lookups (BMGHolder::GetMsgId,
// GetMsgByMsgId, GetFontIndex; the hooks live in vs_racers_option.cpp). Each custom message
// borrows a real message's INF entry (font, attributes) and has its own UTF-16 text in the fixed
// runtime table span. runtime/src/hle/custom_messages.cpp.
namespace CustomMessages {

// The message id showing place `place` as "13th", "21st", ... (13..PlayerSlots::kMaxPlayers), in
// the font of "12th" (Race.bmg 1312). 0 for places outside that range.
uint32_t PlaceBmg(uint32_t place);

// The message id for team colour `colour` (Teams::ColourIndexOf): "Red Team", ..., in the font of
// the player name message (Common.bmg 9501). 0 for an unknown colour.
uint32_t TeamNameBmg(uint32_t colour, bool average);
// A runtime label in the team-name font for colour `colour` (one slot per colour; the text is
// rewritten on every call, at most 23 characters).
uint32_t TeamLabelBmg(uint32_t colour, const std::string& text);

// For BMGHolder::GetMsgId(holder, bmgId): true for a custom id. `fontBmg` is the real message
// whose INF entry it uses; after the game looks that one up, call Index for the msg index to return.
bool IsCustom(uint32_t bmgId, uint32_t& fontBmg);
uint32_t Index(uint32_t bmgId, uint32_t fontMsgIndex);

// For GetMsgByMsgId / GetFontIndex: true for an index Index returned; the text address and the
// msg index whose font entry to use.
bool Text(uint32_t msgIndex, uint32_t& text);
bool FontIndex(uint32_t msgIndex, uint32_t& realMsgIndex);

} // namespace CustomMessages
