// Frame dump (DumpFrame key, F12 by default): writes the state of one frame to
// dump/ (frame.json, screen.png, tiles.png, sprites.png and raw .bin files).
// It never changes the game state: it only reads memory, and the room object
// replay restores g_ram exactly.
#ifndef ZELDA3_FRAME_DUMP_H_
#define ZELDA3_FRAME_DUMP_H_

#include "types.h"

// Ask for a dump. It happens after the next frame that gets drawn.
void FrameDump_Request();
bool FrameDump_IsPending();

// Buffer of width x height 32-bit pixels for the PPU to draw into on the dump
// frame (the locked texture is write-only). Returns NULL on allocation failure.
uint8 *FrameDump_GetScreenBuffer(int width, int height);

// Writes all the files. |pixels| is the frame drawn into the buffer above.
void FrameDump_Write(const uint8 *pixels, int width, int height, int pitch, int render_scale);

// Room objects (frame.json version 3). While a dump is pending, call this after
// each game frame, with the audio mutex held: indoors, it saves all of g_ram,
// replays Dungeon_LoadRoom with the room log on, records which tilemap cells
// each object, door, pushable block and torch changed, and restores g_ram
// byte for byte. Dungeon_LoadRoom and everything it calls only write g_ram.
void FrameDump_CaptureRoomObjects();

// Room log hooks, called from Dungeon_LoadRoom. They do nothing unless
// g_room_log_active is set, which only happens inside the replay above, so the
// game draws exactly the same with or without them.
extern bool g_room_log_active;
enum {
  kRoomLog_Object, kRoomLog_Door, kRoomLog_Block, kRoomLog_Torch,
};
enum {
  kRoomLogPass_Layout, kRoomLogPass_Layer1, kRoomLogPass_Layer2, kRoomLogPass_Layer3,
  kRoomLogPass_Extra,  // pushable blocks and torches
};
void RoomLog_SetPass(int pass);
// |raw| is the entry as stored in the room data (3 bytes for an object, 2 for
// a door, the 16-bit tilemap value for a block or torch).
void RoomLog_Begin(int kind, const uint8 *raw, int len);
void RoomLog_End();

// OAM owners (frame.json version 4): which entity wrote each OAM entry in the
// game frame. While enabled, the hooks below take a copy of the OAM buffer when
// an entity starts drawing and, when it ends, give it the entries that changed
// (a nested scope keeps the entries it claimed). They only read g_ram and write
// to arrays of their own, so the game runs exactly the same with or without
// them. For now it's only enabled on the frame before a dump (FrameDump_
// WantsOamOwners); phase 4 (live UDP) can enable it on every frame with
// OamOwner_SetEnabled(true) and read OamOwner_Entries() after each frame.
enum {
  kOamOwner_Other,     // written outside any hooked scope (or not written)
  kOamOwner_Link,
  kOamOwner_Sprite,    // index = sprite slot k (0-15), type = sprite_type[k]
  kOamOwner_Ancilla,   // index = ancilla slot k (0-9), type = ancilla type
  kOamOwner_Follower,  // type = follower_indicator
};
enum {
  kOamPart_Body,
  kOamPart_Shadow,
};
typedef struct OamOwnerEntry {
  uint8 kind, index, part;
  uint8 type;
} OamOwnerEntry;
void OamOwner_SetEnabled(bool on);
bool OamOwner_IsEnabled();
// True if the owners were recorded for the last game frame (the 128 entries).
bool OamOwner_IsValid();
const OamOwnerEntry *OamOwner_Entries();
// Hooks. BeginFrame runs where the game clears the OAM buffer.
void OamOwner_BeginFrame();
void OamOwner_Push(int kind, int index, int type);
void OamOwner_PushPart(int part);  // same owner as the current scope
void OamOwner_Pop();
// The dump wants the owners of the next frame (a dump is pending), unless the
// environment variable ZELDA3_OAM_OWNERS=0 turns them off (for testing that the
// game runs the same with and without them).
bool FrameDump_WantsOamOwners();

#endif  // ZELDA3_FRAME_DUMP_H_
