#pragma once

// mkw100: which pack build the mod executable runs (the Kamek code digest the mod loader writes to
// 0x800017B0). MKW100's Retro Rewind and Item Fever fixes hold addresses inside one build of each
// (projects/mkwii/mkw100/mod-builds.txt); with any other build they stay off and the game caps that
// mod at 12 racers instead of running unfixed tables past 12.
namespace ModBuild {

enum class Kind { None, RetroRewind, ItemFever, Unknown };

// None for the base game, or before the mod loader has written the digest.
Kind Current();
inline bool IsKnownRetroRewind() { return Current() == Kind::RetroRewind; }
inline bool IsKnownItemFever() { return Current() == Kind::ItemFever; }

} // namespace ModBuild
