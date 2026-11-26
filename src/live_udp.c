// Live sprites for the 3D viewer over UDP. See live_udp.h.
// Everything here only reads emulated memory (g_ram, the PPU's OAM, CGRAM and
// VRAM, and the OAM owner registry), so it can't change game behavior.
//
// Packet (little-endian), version 1:
//   header (52 bytes)
//     0  char[4] "Z3LV"          4  u16 version (1)       6  u16 header size (52)
//     8  u32 session (random per run)                   12  u32 sequence number
//    16  u32 game frame (frame_ctr_dbg, the same as "frame" in a dump)
//    20  u16 room (dungeon_room_index)  22  u8 module  23  u8 submodule
//    24  u8 flags: 1 indoors, 2 Link on the lower level (BG1), 4 OAM owners valid
//    25  u8 main screen (TM)
//    26  i16 BG2 hofs, vofs   30  i16 BG1 hofs, vofs   (the values the PPU gets)
//    34  u8 extra_left, visible left, right, bottom (as in a dump's "screen")
//    38  u8 OAM entries, 39 u8 sprites, 40 u8 ancillas, 41 u8 follower_indicator
//    42  u16 tiles
//    44  Link: u16 x, u16 y, i16 z, u8 floor (lower level), u8 facing
//   then
//     128 x u16   sprite colors (CGRAM 128-255, BGR555)
//     n x 11      OAM entries: u8 i, i16 x, u8 y, u8 tile, u8 attr (bit 0 bank,
//                 1-3 palette, 4-5 priority, 6 hflip, 7 vflip), u8 size (px),
//                 u8 owner kind (0 other, 1 link, 2 sprite, 3 ancilla,
//                 4 follower; 255 = no owners), u8 owner index, u8 owner type,
//                 u8 part (0 body, 1 shadow)
//     n x 9       sprites (sprite_state != 0): u8 slot, u8 type, u8 state,
//                 u16 x, u16 y, u8 z, u8 floor
//     n x 8       ancillas (type != 0): u8 slot, u8 type, u16 x, u16 y, u8 z, u8 floor
//     n x 34      OBJ tiles used by the visible OAM: u16 tile (0-511, = bank*256 +
//                 OAM tile, from VRAM word 0x4000 like a dump's sprites.png),
//                 16 x u16 VRAM words (4bpp: planes 0-1 in words 0-7, 2-3 in 8-15)
// Biggest possible packet: 52 + 256 + 128*11 + 16*9 + 10*8 + 512*34 = 19348 bytes.

// The socket headers go first: on Windows, winsock2.h must come before windows.h.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#endif
typedef SOCKET LiveSocket;
#define kLiveInvalidSocket INVALID_SOCKET
#define LiveCloseSocket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int LiveSocket;
#define kLiveInvalidSocket (-1)
#define LiveCloseSocket close
#endif
#include "live_udp.h"
#include "frame_dump.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "snes/ppu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>


enum {
  kLiveHeaderSize = 52,
  kLiveObjTileVram = 0x4000,
  kLiveMaxPacket = 20000,
};

// Same as kDumpSpriteSizes in frame_dump.c (OBSEL size -> small/large sprite size).
static const uint8 kLiveSpriteSizes[8][2] = {
  {8, 16}, {8, 32}, {8, 64}, {16, 32},
  {16, 64}, {32, 64}, {16, 32}, {16, 32}
};

static bool g_live_enabled;
static LiveSocket g_live_socket = kLiveInvalidSocket;
static struct sockaddr_in g_live_addr;
static uint32 g_live_session, g_live_seq;
static uint8 g_live_packet[kLiveMaxPacket];

static uint8 *Put8(uint8 *p, int v) { *p++ = (uint8)v; return p; }
static uint8 *Put16(uint8 *p, int v) { p[0] = (uint8)v; p[1] = (uint8)(v >> 8); return p + 2; }
static uint8 *Put32(uint8 *p, uint32 v) {
  p[0] = (uint8)v; p[1] = (uint8)(v >> 8); p[2] = (uint8)(v >> 16); p[3] = (uint8)(v >> 24);
  return p + 4;
}

void LiveUdp_Init(bool enabled, int port) {
  g_live_enabled = false;
  if (!enabled)
    return;
  if (port <= 0 || port > 65535)
    port = kLiveUdp_DefaultPort;
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    fprintf(stderr, "LiveUdp: WSAStartup failed, live sprites off\n");
    return;
  }
#endif
  g_live_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_live_socket == kLiveInvalidSocket) {
    fprintf(stderr, "LiveUdp: can't open a UDP socket, live sprites off\n");
    return;
  }
  // Non-blocking: if nobody is listening, or the buffer is full, the packet is dropped.
#ifdef _WIN32
  u_long nonblocking = 1;
  ioctlsocket(g_live_socket, FIONBIO, &nonblocking);
#else
  fcntl(g_live_socket, F_SETFL, fcntl(g_live_socket, F_GETFL, 0) | O_NONBLOCK);
#endif
  memset(&g_live_addr, 0, sizeof(g_live_addr));
  g_live_addr.sin_family = AF_INET;
  g_live_addr.sin_port = htons((uint16)port);
  g_live_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  g_live_session = (uint32)time(NULL) * 2654435761u ^ (uint32)clock();
  g_live_seq = 0;
  g_live_enabled = true;
  printf("LiveUdp: sending live sprites to 127.0.0.1:%d (session %08x)\n", port, g_live_session);
}

bool LiveUdp_IsEnabled() {
  return g_live_enabled;
}

void LiveUdp_Shutdown() {
  if (g_live_socket != kLiveInvalidSocket)
    LiveCloseSocket(g_live_socket);
  g_live_socket = kLiveInvalidSocket;
  g_live_enabled = false;
#ifdef _WIN32
  WSACleanup();
#endif
}

void LiveUdp_SendFrame() {
  if (!g_live_enabled)
    return;
  const Ppu *ppu = g_zenv.ppu;
  const uint16 *oam = ppu->oam;
  bool owners = OamOwner_IsValid();
  uint8 *p = g_live_packet + kLiveHeaderSize;

  // Sprite colors.
  for (int i = 0; i < 128; i++)
    p = Put16(p, ppu->cgram[128 + i]);

  // Visible OAM (same rules as WriteJsonOam in frame_dump.c), and which OBJ tiles it uses.
  uint8 used[512 / 8] = { 0 };
  int num_oam = 0;
  for (int i = 0; i < 128; i++) {
    uint16 w0 = oam[i * 2], w1 = oam[i * 2 + 1];
    int y = w0 >> 8;
    if (y == 0xf0)
      continue;  // hidden
    int high = oam[0x100 + (i >> 3)] >> ((i & 7) * 2);
    int x = (w0 & 0xff) | (high & 1) << 8;
    if (x >= 256 + ppu->extraLeftRight)
      x -= 512;  // same wrap as the PPU
    int tile = w1 & 0xff, bank = (w1 >> 8) & 1;
    int size = kLiveSpriteSizes[ppu->objSize & 7][(high >> 1) & 1];
    int attr = bank | ((w1 >> 9) & 7) << 1 | ((w1 >> 12) & 3) << 4 |
               ((w1 & 0x4000) ? 0x40 : 0) | ((w1 & 0x8000) ? 0x80 : 0);
    p = Put8(p, i);
    p = Put16(p, x);
    p = Put8(p, y);
    p = Put8(p, tile);
    p = Put8(p, attr);
    p = Put8(p, size);
    if (owners) {
      const OamOwnerEntry *o = &OamOwner_Entries()[i];
      p = Put8(p, o->kind);
      p = Put8(p, o->index);
      p = Put8(p, o->type);
      p = Put8(p, o->part);
    } else {
      p = Put8(p, 255), p = Put8(p, 0), p = Put8(p, 0), p = Put8(p, 0);
    }
    num_oam++;
    // A sprite of n x n px uses (n/8)^2 tiles: the column wraps within the row of
    // 16 and the row within the bank, like the PPU.
    for (int dy = 0; dy < size / 8; dy++) {
      for (int dx = 0; dx < size / 8; dx++) {
        int t = bank * 256 + ((((tile >> 4) + dy) & 15) << 4 | ((tile + dx) & 15));
        used[t >> 3] |= 1 << (t & 7);
      }
    }
  }

  // Actors (same as WriteJsonActors in frame_dump.c).
  int num_sprites = 0;
  for (int k = 0; k < 16; k++) {
    if (sprite_state[k] == 0)
      continue;
    p = Put8(p, k);
    p = Put8(p, sprite_type[k]);
    p = Put8(p, sprite_state[k]);
    p = Put16(p, sprite_x_lo[k] | sprite_x_hi[k] << 8);
    p = Put16(p, sprite_y_lo[k] | sprite_y_hi[k] << 8);
    p = Put8(p, sprite_z[k]);
    p = Put8(p, sprite_floor[k]);
    num_sprites++;
  }
  int num_ancillas = 0;
  for (int k = 0; k < 10; k++) {
    if (ancilla_type[k] == 0)
      continue;
    p = Put8(p, k);
    p = Put8(p, ancilla_type[k]);
    p = Put16(p, ancilla_x_lo[k] | ancilla_x_hi[k] << 8);
    p = Put16(p, ancilla_y_lo[k] | ancilla_y_hi[k] << 8);
    p = Put8(p, ancilla_z[k]);
    p = Put8(p, ancilla_floor[k]);
    num_ancillas++;
  }

  // Pixels of the OBJ tiles in use, straight from VRAM.
  int num_tiles = 0;
  for (int t = 0; t < 512; t++) {
    if (!(used[t >> 3] & (1 << (t & 7))))
      continue;
    p = Put16(p, t);
    const uint16 *src = &g_zenv.vram[(kLiveObjTileVram + t * 16) & 0x7fff];
    for (int w = 0; w < 16; w++)
      p = Put16(p, src[w]);
    num_tiles++;
  }

  // Header.
  uint8 *h = g_live_packet;
  memcpy(h, "Z3LV", 4);
  Put16(h + 4, 1);
  Put16(h + 6, kLiveHeaderSize);
  Put32(h + 8, g_live_session);
  Put32(h + 12, g_live_seq++);
  Put32(h + 16, (uint32)frame_ctr_dbg);
  Put16(h + 20, dungeon_room_index);
  Put8(h + 22, main_module_index);
  Put8(h + 23, submodule_index);
  Put8(h + 24, (player_is_indoors ? 1 : 0) | (link_is_on_lower_level ? 2 : 0) | (owners ? 4 : 0));
  Put8(h + 25, TM_copy);
  Put16(h + 26, BG2HOFS_copy);
  Put16(h + 28, BG2VOFS_copy);
  Put16(h + 30, BG1HOFS_copy);
  Put16(h + 32, BG1VOFS_copy);
  Put8(h + 34, ppu->extraLeftRight);
  Put8(h + 35, ppu->extraLeftCur);
  Put8(h + 36, ppu->extraRightCur);
  Put8(h + 37, ppu->extraBottomCur);
  Put8(h + 38, num_oam);
  Put8(h + 39, num_sprites);
  Put8(h + 40, num_ancillas);
  Put8(h + 41, follower_indicator);
  Put16(h + 42, num_tiles);
  Put16(h + 44, link_x_coord);
  Put16(h + 46, link_y_coord);
  Put16(h + 48, link_z_coord);
  Put8(h + 50, link_is_on_lower_level);
  Put8(h + 51, link_direction_facing);

  sendto(g_live_socket, (const char *)g_live_packet, (int)(p - g_live_packet), 0,
         (const struct sockaddr *)&g_live_addr, sizeof(g_live_addr));
}
