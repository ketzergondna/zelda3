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

#endif  // ZELDA3_FRAME_DUMP_H_
