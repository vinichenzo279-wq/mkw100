// mkw100: the HUD's name balloons (name tags) in races of more than 12 players.
//
// Each race HUD owns a RaceBalloons object (ctor 0x807F1C58, 0xC4 bytes): up to 3 name balloon
// controls, the players they show (+0x14), `Vec3 positions[12]` at +0x20 and more state after it
// (+0xB0, the item balloon at +0xC0). RaceBalloons::Update (0x807F132C) walks every race player
// (`i < RaceConfig race player count`), stores each one's position in positions[i] and picks the
// nearest ones in view for the balloons. Players 12+ wrote past the array into the next heap block
// (24 players: the first name balloon control, then a call through float bytes from
// ControlGroup::Draw).
//
// The array cannot grow in place (the fields after it are the class's own), and there is one object
// per race HUD screen, so it is not a singleton array class. Every access forms the element's address
// as `add rD, object, i * 12` and then reads or writes +0x20: those four adds (0x807F1128, 0x807F1758,
// 0x807F1AD8, 0x807F1DA8) are `objadd` sites in projects/mkwii/mkw100/array-sites.txt, which send
// players 12+ to a shadow table per object (MKW100_ObjectArrayAdd, at 0x82D10000). Update then runs
// over every racer, so any racer near the camera can get a name tag. (Until 2026-11-13 Update ran with
// the player count lowered to 12, and only players 0..11 ever got one.)
//
// No hooks remain here; the file keeps the notes next to the other HUD files.
