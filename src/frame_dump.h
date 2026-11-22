// Frame dump (DumpFrame key, F12 by default): writes the state of one frame to
// dump/ (frame.json, screen.png, tiles.png, sprites.png and raw .bin files).
// It only reads memory, it never changes the game state.
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

#endif  // ZELDA3_FRAME_DUMP_H_
