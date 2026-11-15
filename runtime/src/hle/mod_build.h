#pragma once

#include <cstdint>

// mkw100: which pack build the mod executable runs (the Kamek code digest the mod loader writes to
// 0x800017B0). MKW100's Retro Rewind and Item Fever fixes hold addresses inside one build of each
// (projects/mkwii/mkw100/mod-builds.txt); with any other build they stay off and the game caps that
// mod at 12 racers instead of running unfixed tables past 12.
namespace ModBuild {

// RetroRewindItemFever: the combined module of Retro Rewind and Item Fever's items
// (scripts/mkw100/combine_pul.py, docs/mkw100/rr-item-fever.md), made from the two builds below;
// Retro Rewind's code sits at the module base and Item Fever's kItemFeverCombinedOffset above it.
enum class Kind { None, RetroRewind, ItemFever, RetroRewindItemFever, Unknown };

inline constexpr uint32_t kItemFeverCombinedOffset = 0xC0000;

// None for the base game, or before the mod loader has written the digest.
Kind Current();
inline bool IsKnownRetroRewind() { return Current() == Kind::RetroRewind; }
inline bool IsKnownItemFever() { return Current() == Kind::ItemFever; }
// The measured Retro Rewind code at the module base: Retro Rewind alone or combined.
inline bool HasKnownRetroRewindCode() {
    const Kind k = Current();
    return k == Kind::RetroRewind || k == Kind::RetroRewindItemFever;
}
// The measured Item Fever code, alone or combined; ItemFeverOffset says where it sits relative to
// the addresses measured in the Item Fever build.
inline bool HasKnownItemFeverCode() {
    const Kind k = Current();
    return k == Kind::ItemFever || k == Kind::RetroRewindItemFever;
}
inline uint32_t ItemFeverOffset() { return Current() == Kind::RetroRewindItemFever ? kItemFeverCombinedOffset : 0; }

} // namespace ModBuild
