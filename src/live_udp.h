// Live sprites for the 3D viewer (phase 4). When [Viewer] LiveUdp = 1 in
// zelda3.ini, after each drawn frame it sends one UDP packet to
// 127.0.0.1:LiveUdpPort with what the viewer needs to draw the sprites live:
// the visible OAM with its owners, the actors, the 128 sprite colors and the
// pixels of every OBJ tile that the visible OAM uses (each packet stands on its
// own: a lost packet only costs that frame). The room still comes from the frame
// dump (F12). Like the dump, it only reads memory, so the game runs exactly the
// same with it on or off. Packet format: see "Formato de datos" in CLAUDE.md
// (and live_udp.c).
#ifndef ZELDA3_LIVE_UDP_H_
#define ZELDA3_LIVE_UDP_H_

#include "types.h"

enum { kLiveUdp_DefaultPort = 47103 };

// Opens the socket if |enabled|. Prints what it did.
void LiveUdp_Init(bool enabled, int port);
bool LiveUdp_IsEnabled();
// Builds and sends the packet of the frame just drawn (call after drawing it).
void LiveUdp_SendFrame();
void LiveUdp_Shutdown();

#endif  // ZELDA3_LIVE_UDP_H_
